
#ifndef __MEMCACHED_HANDLER_IMPL_HPP__
#define __MEMCACHED_HANDLER_IMPL_HPP__

#include <string.h>
#include "cpu_context.hpp"
#include "event_queue.hpp"
#include "request_handler/memcached_handler.hpp"
#include "conn_fsm.hpp"
#include "corefwd.hpp"

#define DELIMS " \t\n\r"
#define MALFORMED_RESPONSE "ERROR\r\n"
#define UNIMPLEMENTED_RESPONSE "SERVER_ERROR functionality not supported\r\n"
#define STORAGE_SUCCESS "STORED\r\n"
#define RETRIEVE_TERMINATOR "END\r\n"

// TODO: if we receive a small request from the user that can be
// satisfied on the same CPU, we should probably special case it and
// do it right away, no need to send it to itself, process it, and
// then send it back to itself.

// Process commands received from the user
template<class config_t>
typename memcached_handler_t<config_t>::parse_result_t memcached_handler_t<config_t>::parse_request(event_t *event)
{
    conn_fsm_t *fsm = (conn_fsm_t*)event->state;
    char *buf = fsm->buf;
    unsigned int size = fsm->nbuf;

    // TODO: we might end up getting a command, and a piece of the
    // next command. It also means that we can't use one buffer
    //for both recv and send, we need to add a send buffer
    // (assuming we want to support out of band  commands).
    
    // Find the first line in the buffer
    char *line_end = (char *)memchr(buf, '\n', size);
    if (line_end == NULL)   //make sure \n is in the buffer
        return req_handler_t::op_partial_packet;    //if \n is at the beginning of the buffer, or if it is not preceeded by \r, the request is malformed

    if (loading_data)
        return read_data(buf, size, fsm);

    if (line_end == buf || line_end[-1] != '\r')
        return malformed_request(fsm);

    // if we're not reading a binary blob, then the line will be a string - let's null terminate it
    *line_end = '\0';
    unsigned int line_len = line_end - buf + 1;

    // get the first token to determine the command
    char *state;
    char *cmd_str = strtok_r(buf, DELIMS, &state);

    if(cmd_str == NULL)
        return malformed_request(fsm);
    
    // Execute command
    if(!strcmp(cmd_str, "quit")) {
        // Make sure there's no more tokens
        if (strtok_r(NULL, DELIMS, &state))  //strtok will return NULL if there are no more tokens
            return malformed_request(fsm);
        // Quit the connection
        return req_handler_t::op_req_quit;

    } else if(!strcmp(cmd_str, "shutdown")) {
        // Make sure there's no more tokens
        if (strtok_r(NULL, DELIMS, &state))  //strtok will return NULL if there are no more tokens
            return malformed_request(fsm);
        // Shutdown the server
        return req_handler_t::op_req_shutdown;

    } else if(!strcmp(cmd_str, "set")) {     // check for storage commands
            return parse_storage_command(SET, state, line_len, fsm);
    } else if(!strcmp(cmd_str, "add")) {
            return parse_storage_command(ADD, state, line_len, fsm);
    } else if(!strcmp(cmd_str, "replace")) {
            return parse_storage_command(REPLACE, state, line_len, fsm);
    } else if(!strcmp(cmd_str, "append")) {
            return parse_storage_command(APPEND, state, line_len, fsm);
    } else if(!strcmp(cmd_str, "prepend")) {
            return parse_storage_command(PREPEND, state, line_len, fsm);
    } else if(!strcmp(cmd_str, "cas")) {
            return parse_storage_command(CAS, state, line_len, fsm);

    } else if(!strcmp(cmd_str, "get")) {    // check for retrieval commands
            return get(state, false, fsm);
    } else if(!strcmp(cmd_str, "gets")) {
            return get(state, true, fsm);

    } else if(!strcmp(cmd_str, "delete")) {
        return remove(state, fsm);

    } else if(!strcmp(cmd_str, "incr")) {
        return adjust(state, true, fsm);
    } else if(!strcmp(cmd_str, "decr")) {
        return adjust(state, false, fsm);
    } else {
        // Invalid command
        return malformed_request(fsm);
    }
}

template <class config_t>
typename memcached_handler_t<config_t>::parse_result_t memcached_handler_t<config_t>::parse_storage_command(storage_command command, char *state, unsigned int line_len, conn_fsm_t *fsm) {
    char *key_tmp = strtok_r(NULL, DELIMS, &state);
    char *flags_str = strtok_r(NULL, DELIMS, &state);
    char *exptime_str = strtok_r(NULL, DELIMS, &state);
    char *bytes_str = strtok_r(NULL, DELIMS, &state);
    char *cas_unique_str = NULL;
    if (command == CAS)
        cas_unique_str = strtok_r(NULL, DELIMS, &state);
    char *noreply_str = strtok_r(NULL, DELIMS, &state); //optional

    if (key_tmp == NULL || flags_str == NULL || exptime_str == NULL || bytes_str == NULL || (command == CAS && cas_unique_str == NULL)) //check for proper number of arguments
        return malformed_request(fsm);

    cmd = command;
    key = strdup(key_tmp); //TODO: Consider not allocating with malloc

    char *invalid_char;
    flags = strtoul(flags_str, &invalid_char, 10);  //a 32 bit integer.  int alone does not guarantee 32 bit length
    if (*invalid_char != '\0')  // ensure there were no improper characters in the token - i.e. parse was successful
        return malformed_request(fsm);

    exptime = strtoul(exptime_str, &invalid_char, 10);
    if (*invalid_char != '\0')
        return malformed_request(fsm);

    bytes = strtoul(bytes_str, &invalid_char, 10);
    if (*invalid_char != '\0')
        return malformed_request(fsm);

    if (cmd == CAS) {
        cas_unique = strtoull(cas_unique_str, &invalid_char, 10);
        if (*invalid_char != '\0')
            return malformed_request(fsm);
    }

    noreply = false;
    if (noreply_str != NULL) {
        if (!strcmp(noreply_str, "noreply")) {
            noreply = true;
        } else {
            return malformed_request(fsm);
        }
    }

    fsm->consume(line_len); //consume the line
    loading_data = true;

    return read_data(fsm->buf, fsm->nbuf, fsm);
}
	
template <class config_t>
typename memcached_handler_t<config_t>::parse_result_t memcached_handler_t<config_t>::read_data(char *data, unsigned int size, conn_fsm_t *fsm) {
    check("memcached handler should be in loading data state", !loading_data);
    if (size < bytes + 2){//check that the buffer contains enough data.  must also include \r\n
        return req_handler_t::op_partial_packet;
    }

    loading_data = false;
    parse_result_t ret;
    switch(cmd) {
        case SET:
            ret = set(data, fsm);
            break;
        case ADD:
            ret = add(data, fsm);
            break;
        case REPLACE:
            ret = replace(data, fsm);
            break;
        case APPEND:
            ret = append(data, fsm);
            break;
        case PREPEND:
            ret = prepend(data, fsm);
            break;
        case CAS:
            ret = prepend(data, fsm);
            break;
        default:
            ret = malformed_request(fsm);
            break;
    }

    //free stored state
    free(key);

    return ret;
}

template <class config_t>
typename memcached_handler_t<config_t>::parse_result_t memcached_handler_t<config_t>::set(char *data, conn_fsm_t *fsm) {
    //TODO: For now, we assume the data is an integer, because that is the only data type the database can handle
    data[bytes] = '\0'; //null terminate string
    char *invalid_char;
    unsigned int value_int = (int)strtoul(data, &invalid_char, 10);
    if (*invalid_char != '\0')  // ensure there were no improper characters in the token - i.e. parse was successful
        return unimplemented_request(fsm);


    set_key(fsm, btree::str_to_key(key), value_int);

    fsm->consume(bytes+2);
    return req_handler_t::op_req_complex;
}

template <class config_t>
typename memcached_handler_t<config_t>::parse_result_t memcached_handler_t<config_t>::add(char *data, conn_fsm_t *fsm) {
    return unimplemented_request(fsm);
}

template <class config_t>
typename memcached_handler_t<config_t>::parse_result_t memcached_handler_t<config_t>::replace(char *data, conn_fsm_t *fsm) {
    return unimplemented_request(fsm);
}

template <class config_t>
typename memcached_handler_t<config_t>::parse_result_t memcached_handler_t<config_t>::append(char *data, conn_fsm_t *fsm) {
    return unimplemented_request(fsm);
}

template <class config_t>
typename memcached_handler_t<config_t>::parse_result_t memcached_handler_t<config_t>::prepend(char *data, conn_fsm_t *fsm) {
    return unimplemented_request(fsm);
}

template <class config_t>
typename memcached_handler_t<config_t>::parse_result_t memcached_handler_t<config_t>::cas(char *data, conn_fsm_t *fsm) {
    return unimplemented_request(fsm);
}

template <class config_t>
typename memcached_handler_t<config_t>::parse_result_t memcached_handler_t<config_t>::malformed_request(conn_fsm_t *fsm) {
    write_msg(fsm, MALFORMED_RESPONSE);
    return req_handler_t::op_malformed;
}

template <class config_t>
typename memcached_handler_t<config_t>::parse_result_t memcached_handler_t<config_t>::unimplemented_request(conn_fsm_t *fsm) {
    write_msg(fsm, UNIMPLEMENTED_RESPONSE);
    return req_handler_t::op_malformed;
}

template <class config_t>
void memcached_handler_t<config_t>::write_msg(conn_fsm_t *fsm, const char *str) {
    int len = strlen(str);
    memcpy(fsm->buf, str, len+1);
    fsm->nbuf = len+1;
}

template<class config_t> void memcached_handler_t<config_t>::set_key(conn_fsm_t *fsm, btree_key *key, int value){
    btree_set_fsm_t *btree_fsm = new btree_set_fsm_t(get_cpu_context()->event_queue->cache);
    btree_fsm->init_update(key, value);
    req_handler_t::event_queue->message_hub.store_message(key_to_cpu(key, req_handler_t::event_queue->nqueues),
            btree_fsm);

    // Create request
    request_t *request = new request_t(fsm);
    request->fsms[request->nstarted] = btree_fsm;
    request->nstarted++;
    fsm->current_request = request;
    btree_fsm->request = request;
}


template <class config_t>
typename memcached_handler_t<config_t>::parse_result_t memcached_handler_t<config_t>::get(char *state, bool include_unique, conn_fsm_t *fsm) {
    char *key_str = strtok_r(NULL, DELIMS, &state);
    if (key_str == NULL)
        return malformed_request(fsm);

    if (include_unique)
        return unimplemented_request(fsm);

    // Create request
    request_t *request = new request_t(fsm);
    do {
        // See if we can fit one more request
        if(request->nstarted == MAX_OPS_IN_REQUEST) {
            // We can't fit any more operations, let's just break
            // and complete the ones we already sent out to other
            // cores.
            break;

            // TODO: to a user, it will look like some of his
            // requests aren't satisfied. We need to notify them
            // somehow.
        }

        btree_key *key = btree::str_to_key(key_str);

        // Ok, we've got a key, initialize the FSM and add it to
        // the request
        btree_get_fsm_t *btree_fsm = new btree_get_fsm_t(get_cpu_context()->event_queue->cache);
        btree_fsm->request = request;
        btree_fsm->init_lookup(key);
        request->fsms[request->nstarted] = btree_fsm;
        request->nstarted++;

        // Add the fsm to appropriate queue
        req_handler_t::event_queue->message_hub.store_message(key_to_cpu(key, req_handler_t::event_queue->nqueues), btree_fsm);
        key_str = strtok_r(NULL, DELIMS, &state);
    } while(key_str);

    // Set the current request in the connection fsm
    fsm->current_request = request;
    return req_handler_t::op_req_complex;
}


template <class config_t>
typename memcached_handler_t<config_t>::parse_result_t memcached_handler_t<config_t>::remove(char *state, conn_fsm_t *fsm) {
    char *key_str = strtok_r(NULL, DELIMS, &state);
    if (key_str == NULL)
        return malformed_request(fsm);

    unsigned long time = 0;
    bool noreply = false;
    char *time_or_noreply_str = strtok_r(NULL, DELIMS, &state);
    if (time_or_noreply_str != NULL) {
        if (!strcmp(time_or_noreply_str, "noreply")) {
            noreply = true;
        } else { //must represent a time, then
            char *invalid_char;
            time = strtoul(time_or_noreply_str, &invalid_char, 10);
            if (*invalid_char != '\0')  // ensure there were no improper characters in the token - i.e. parse was successful
                return unimplemented_request(fsm);

            // see if there's a noreply arg too
            char *noreply_str = strtok_r(NULL, DELIMS, &state);
            if (noreply_str != NULL) {
                if (!strcmp(noreply_str, "noreply")) {
                    noreply = true;
                } else {
                    return malformed_request(fsm);
                }
            }
        }
    }

    // parsed successfully, but functionality not yet implemented
    return unimplemented_request(fsm);
    /*
    request_t *request = new request_t(fsm);

    do {
        if(request->nstarted == MAX_OPS_IN_REQUEST) {
            break;
        }

        int key_int = atoi(key_str);
        btree_delete_fsm_t *btree_fsm = new btree_delete_fsm_t(get_cpu_context()->event_queue->cache);
        btree_fsm->request = request;
        btree_fsm->init_delete(key_int);
        request->fsms[request->nstarted] = btree_fsm;
        request->nstarted++;

        req_handler_t::event_queue->message_hub.store_message(key_to_cpu(key_int, req_handler_t::event_queue->nqueues), btree_fsm);
        key_str = strtok_r(NULL, DELIMS, &state);
    } while(key_str);

    fsm->current_request = request;
    return req_handler_t::op_req_complex;
    */
}

template <class config_t>
typename memcached_handler_t<config_t>::parse_result_t memcached_handler_t<config_t>::adjust(char *state, bool inc, conn_fsm_t *fsm) {
    char *key_str = strtok_r(NULL, DELIMS, &state);
    char *value_str = strtok_r(NULL, DELIMS, &state);
    if (key_str == NULL || value_str == NULL)
        return malformed_request(fsm);

    bool noreply = false;
    char *noreply_str = strtok_r(NULL, DELIMS, &state);
    if (noreply_str != NULL) {
        if (!strcmp(noreply_str, "noreply")) {
            noreply = true;
        } else {
            return malformed_request(fsm);
        }
    }

    // parsed successfully, but functionality not yet implemented
    return unimplemented_request(fsm);
}
    
template<class config_t>
void memcached_handler_t<config_t>::build_response(request_t *request) {
    // Since we're in the middle of processing a command,
    // fsm->buf must exist at this point.
    conn_fsm_t *fsm = request->netfsm;
    btree_get_fsm_t *btree_get_fsm = NULL;
    btree_set_fsm_t *btree_set_fsm = NULL;
    char *buf = fsm->buf;
    fsm->nbuf = 0;
    int count;
    char value_str[15];
    
    assert(request->nstarted > 0 && request->nstarted == request->ncompleted);
    switch(request->fsms[0]->fsm_type) {
    case btree_fsm_t::btree_get_fsm:
        // TODO: make sure we don't overflow the buffer with sprintf
        for(unsigned int i = 0; i < request->nstarted; i++) {
            btree_get_fsm = (btree_get_fsm_t*)request->fsms[i];
            if(btree_get_fsm->op_result == btree_get_fsm_t::btree_found) {
                int value_len = sprintf(value_str, "%d", btree_get_fsm->value);

                //TODO: support flags
                btree_key *key = btree_get_fsm->key;
                count = sprintf(buf, "VALUE %*.*s %u %u\r\n%s\r\n", key->size, key->size, key->contents, 0, value_len, value_str);
                fsm->nbuf += count;
                buf += count;
            } else if(btree_get_fsm->op_result == btree_get_fsm_t::btree_not_found) {
                // do nothing
            }
            delete btree_get_fsm;
        }
        count = sprintf(buf, RETRIEVE_TERMINATOR);
        fsm->nbuf += count;
        break;

    case btree_fsm_t::btree_set_fsm:
        // For now we only support one set operation at a time
        assert(request->nstarted == 1);

        btree_set_fsm = (btree_set_fsm_t*)request->fsms[0];
        if (!noreply) {
            strcpy(buf, STORAGE_SUCCESS);
            fsm->nbuf = strlen(STORAGE_SUCCESS);
        } else {
            fsm->nbuf = 0;
        }
        delete btree_set_fsm;
        break;

    default:
        check("memcached_handler_t::build_response - Unknown btree op", 1);
        break;
    }
    delete request;
    fsm->current_request = NULL;
}

#endif // __MEMCACHED_HANDLER_IMPL_HPP__
