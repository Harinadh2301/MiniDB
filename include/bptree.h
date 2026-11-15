#ifndef MINIDB_BPTREE_H
#define MINIDB_BPTREE_H

#include "error.h"
#include "record.h"

#include <stdint.h>

#ifndef BT_ORDER
#define BT_ORDER 4
#endif

#if BT_ORDER < 3
#error "BT_ORDER must be at least 3"
#endif

#define INTERNAL_MAX_KEYS (BT_ORDER - 1)
#define INTERNAL_MAX_CHILDREN BT_ORDER
#define LEAF_MAX_KEYS (BT_ORDER - 1)

typedef struct {
    uint32_t slot;
} RecordRef;

typedef struct BPlusTree BPlusTree;

typedef struct {
    const void *leaf;
    int index;
} BPIter;

BPlusTree *bptree_create(void);
DbStatus bptree_insert(BPlusTree *tree, const char *key, RecordRef ref);
DbStatus bptree_search(const BPlusTree *tree, const char *key, RecordRef *out);
DbStatus bptree_delete(BPlusTree *tree, const char *key);
DbStatus bptree_iter_first(const BPlusTree *tree, BPIter *iterator);
DbStatus bptree_iter_seek(const BPlusTree *tree, const char *low, BPIter *iterator);
int bptree_iter_next(BPIter *iterator, char key[MAX_TAG_LENGTH], RecordRef *ref);
DbStatus bptree_check(const BPlusTree *tree);
void bptree_destroy(BPlusTree *tree);

#endif
