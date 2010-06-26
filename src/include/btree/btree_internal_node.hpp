#ifndef __BTREE_INTERNAL_NODE_HPP__
#define __BTREE_INTERNAL_NODE_HPP__

#include "utils.hpp"
#include "btree/btree_node.hpp"

#define byte char
#define MAX_KEY_SIZE 250

struct btree_internal_blob {
    off64_t lnode;
    btree_key key;
};


struct btree_internal_node {
    bool leaf;
    uint16_t nblobs;
    uint16_t frontmost_offset;
    uint16_t blob_offsets[0];
};

typedef btree_internal_node internal_node_t;

//TODO: rename
class btree_str_key_comp;

class btree_internal : public btree {
    friend class btree_str_key_comp;
    public:
    static void init(btree_internal_node *node);
    static void init(btree_internal_node *node, btree_internal_node *lnode, uint16_t *offsets, int numblobs);

    static int insert(btree_internal_node *node, btree_key *key, off64_t lnode, off64_t rnode);
    static off64_t lookup(btree_internal_node *node, btree_key *key);
    static void split(btree_internal_node *node, btree_internal_node *rnode, btree_key **median);
    static bool remove(btree_internal_node *node, btree_key *key);

    static bool is_full(btree_internal_node *node);

    protected:
    static size_t blob_size(btree_internal_blob *blob);
    static btree_internal_blob *get_blob(btree_internal_node *node, uint16_t offset);
    static void delete_blob(btree_internal_node *node, uint16_t offset);
    static uint16_t insert_blob(btree_internal_node *node, btree_internal_blob *blob);
    static uint16_t insert_blob(btree_internal_node *node, off64_t lnode, btree_key *key);
    static int get_offset_index(btree_internal_node *node, btree_key *key);
    static void delete_offset(btree_internal_node *node, int index);
    static void insert_offset(btree_internal_node *node, uint16_t offset, int index);
    static void make_last_blob_special(btree_internal_node *node);
    static bool is_equal(btree_key *key1, btree_key *key2);
};

class btree_str_key_comp {
    btree_internal_node *node;
    btree_key *key;
    public:
    btree_str_key_comp(btree_internal_node *_node) : node(_node), key(NULL)  { };
    btree_str_key_comp(btree_internal_node *_node, btree_key *_key) : node(_node), key(_key)  { };
    bool operator()(const uint16_t offset1, const uint16_t offset2) {
        btree_key *key1 = offset1 == 0 ? key : &btree_internal::get_blob(node, offset1)->key;
        btree_key *key2 = offset2 == 0 ? key : &btree_internal::get_blob(node, offset2)->key;
        int cmp;
        if (key1->size == 0 && key2->size == 0) //check for the special end blob
            cmp = 0;
        else if (key1->size == 0)
            cmp = 1;
        else if (key2->size == 0)
            cmp = -1;
        else
            cmp = sized_strcmp(key1->contents, key1->size, key2->contents, key2->size);

        return cmp < 0;
    }
};



#endif // __BTREE_INTERNAL_NODE_HPP__
