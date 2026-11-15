#include "bptree.h"

#include <stdlib.h>
#include <string.h>

typedef struct BPlusNode BPlusNode;

struct BPlusNode {
    int is_leaf;
    int key_count;
    union {
        struct {
            char keys[INTERNAL_MAX_KEYS + 1][MAX_TAG_LENGTH];
            BPlusNode *children[INTERNAL_MAX_CHILDREN + 1];
        } in;
        struct {
            char keys[LEAF_MAX_KEYS + 1][MAX_TAG_LENGTH];
            RecordRef refs[LEAF_MAX_KEYS + 1];
            BPlusNode *next;
        } leaf;
    } u;
};

struct BPlusTree {
    BPlusNode *root;
};

typedef struct {
    BPlusNode *node;
    int child_index;
} PathEntry;

typedef struct {
    int depth;
    int leaf_depth;
    BPlusNode *previous_leaf;
} CheckState;

static BPlusNode *node_create(int is_leaf)
{
    BPlusNode *node = (BPlusNode *)calloc(1, sizeof(*node));
    if (node != NULL) node->is_leaf = is_leaf;
    return node;
}

static void node_destroy(BPlusNode *node)
{
    int i;
    if (node == NULL) return;
    if (!node->is_leaf) {
        for (i = 0; i <= node->key_count; ++i) {
            node_destroy(node->u.in.children[i]);
        }
    }
    free(node);
}

BPlusTree *bptree_create(void)
{
    return (BPlusTree *)calloc(1, sizeof(BPlusTree));
}

static int child_for_key(const BPlusNode *node, const char *key)
{
    int index = 0;
    while (index < node->key_count
        && strcmp(key, node->u.in.keys[index]) >= 0) ++index;
    return index;
}

static BPlusNode *find_leaf(const BPlusTree *tree, const char *key,
    PathEntry *path, int *path_length)
{
    BPlusNode *node;
    int depth = 0;

    if (tree == NULL || path_length == NULL) return NULL;
    node = tree->root;
    while (node != NULL && !node->is_leaf) {
        int index;
        if (depth >= 256) return NULL;
        index = child_for_key(node, key);
        if (path != NULL) {
            path[depth].node = node;
            path[depth].child_index = index;
        }
        ++depth;
        node = node->u.in.children[index];
    }
    *path_length = depth;
    return node;
}

static int leaf_lower_bound(const BPlusNode *leaf, const char *key)
{
    int low = 0;
    int high = leaf->key_count;
    while (low < high) {
        int middle = low + (high - low) / 2;
        if (strcmp(leaf->u.leaf.keys[middle], key) < 0) low = middle + 1;
        else high = middle;
    }
    return low;
}

DbStatus bptree_search(const BPlusTree *tree, const char *key, RecordRef *out)
{
    BPlusNode *leaf;
    int depth;
    int index;

    if (tree == NULL || out == NULL || key == NULL || key[0] == '\0'
        || strlen(key) >= MAX_TAG_LENGTH) return DB_ERR_INVALID;
    leaf = find_leaf(tree, key, NULL, &depth);
    if (leaf == NULL) return DB_ERR_NOT_FOUND;
    index = leaf_lower_bound(leaf, key);
    if (index == leaf->key_count || strcmp(leaf->u.leaf.keys[index], key) != 0) {
        return DB_ERR_NOT_FOUND;
    }
    *out = leaf->u.leaf.refs[index];
    return DB_OK;
}

static void free_node_pool(BPlusNode **pool, int count)
{
    int i;
    for (i = 0; i < count; ++i) free(pool[i]);
}

static BPlusNode *take_node(BPlusNode **pool, int *pool_count, int is_leaf)
{
    BPlusNode *node = pool[--(*pool_count)];
    memset(node, 0, sizeof(*node));
    node->is_leaf = is_leaf;
    return node;
}

static const char *refresh_separators(BPlusNode *node)
{
    int i;
    const char *minimum;

    if (node->is_leaf) return node->u.leaf.keys[0];
    minimum = refresh_separators(node->u.in.children[0]);
    for (i = 1; i <= node->key_count; ++i) {
        strcpy(node->u.in.keys[i - 1],
            refresh_separators(node->u.in.children[i]));
    }
    return minimum;
}

DbStatus bptree_insert(BPlusTree *tree, const char *key, RecordRef ref)
{
    PathEntry path[256];
    BPlusNode *pool[258];
    BPlusNode *leaf;
    BPlusNode *right;
    int depth;
    int pool_count = 0;
    int index;
    int i;
    char split_key[MAX_TAG_LENGTH];

    if (tree == NULL || key == NULL || key[0] == '\0'
        || strlen(key) >= MAX_TAG_LENGTH) return DB_ERR_INVALID;
    if (tree->root == NULL) {
        tree->root = node_create(1);
        if (tree->root == NULL) return DB_ERR_NOMEM;
        strcpy(tree->root->u.leaf.keys[0], key);
        tree->root->u.leaf.refs[0] = ref;
        tree->root->key_count = 1;
        return DB_OK;
    }

    leaf = find_leaf(tree, key, path, &depth);
    if (leaf == NULL) return DB_ERR_INTERNAL;
    index = leaf_lower_bound(leaf, key);
    if (index < leaf->key_count && strcmp(leaf->u.leaf.keys[index], key) == 0) {
        return DB_ERR_DUPLICATE;
    }

    if (leaf->key_count == LEAF_MAX_KEYS) {
        ++pool_count;
        for (i = depth - 1; i >= 0; --i) {
            if (path[i].node->key_count != INTERNAL_MAX_KEYS) break;
            ++pool_count;
        }
        if (i < 0) ++pool_count;
    }
    for (i = 0; i < pool_count; ++i) {
        pool[i] = node_create(0);
        if (pool[i] == NULL) {
            free_node_pool(pool, i);
            return DB_ERR_NOMEM;
        }
    }

    memmove(leaf->u.leaf.keys + index + 1, leaf->u.leaf.keys + index,
        (size_t)(leaf->key_count - index) * sizeof(leaf->u.leaf.keys[0]));
    memmove(leaf->u.leaf.refs + index + 1, leaf->u.leaf.refs + index,
        (size_t)(leaf->key_count - index) * sizeof(leaf->u.leaf.refs[0]));
    memmove(leaf->u.leaf.keys[index], key, strlen(key) + 1);
    leaf->u.leaf.refs[index] = ref;
    ++leaf->key_count;
    if (leaf->key_count <= LEAF_MAX_KEYS) return DB_OK;

    right = take_node(pool, &pool_count, 1);
    {
        int total = leaf->key_count;
        int left_count = (total + 1) / 2;
        int right_count = total - left_count;
        memcpy(right->u.leaf.keys, leaf->u.leaf.keys + left_count,
            (size_t)right_count * sizeof(leaf->u.leaf.keys[0]));
        memcpy(right->u.leaf.refs, leaf->u.leaf.refs + left_count,
            (size_t)right_count * sizeof(leaf->u.leaf.refs[0]));
        right->key_count = right_count;
        leaf->key_count = left_count;
        right->u.leaf.next = leaf->u.leaf.next;
        leaf->u.leaf.next = right;
    }
    strcpy(split_key, right->u.leaf.keys[0]);

    for (i = depth - 1; i >= 0; --i) {
        BPlusNode *parent = path[i].node;
        int child_index = path[i].child_index;
        int j;

        memmove(parent->u.in.keys + child_index + 1,
            parent->u.in.keys + child_index,
            (size_t)(parent->key_count - child_index) * sizeof(parent->u.in.keys[0]));
        memmove(parent->u.in.children + child_index + 2,
            parent->u.in.children + child_index + 1,
            (size_t)(parent->key_count - child_index)
                * sizeof(parent->u.in.children[0]));
        strcpy(parent->u.in.keys[child_index], split_key);
        parent->u.in.children[child_index + 1] = right;
        ++parent->key_count;
        if (parent->key_count <= INTERNAL_MAX_KEYS) {
            free_node_pool(pool, pool_count);
            return DB_OK;
        }

        {
            int total = parent->key_count;
            int middle = total / 2;
            int right_keys = total - middle - 1;
            char promoted[MAX_TAG_LENGTH];
            strcpy(promoted, parent->u.in.keys[middle]);
            BPlusNode *new_right = take_node(pool, &pool_count, 0);

            new_right->key_count = right_keys;
            for (j = 0; j <= right_keys; ++j) {
                new_right->u.in.children[j] = parent->u.in.children[middle + 1 + j];
            }
            if (right_keys > 0) {
                memcpy(new_right->u.in.keys,
                    parent->u.in.keys + middle + 1,
                    (size_t)right_keys * sizeof(parent->u.in.keys[0]));
            }
            parent->key_count = middle;
            right = new_right;
            if (i == 0) {
                BPlusNode *new_root = take_node(pool, &pool_count, 0);
                new_root->key_count = 1;
                strcpy(new_root->u.in.keys[0], promoted);
                new_root->u.in.children[0] = parent;
                new_root->u.in.children[1] = right;
                tree->root = new_root;
                free_node_pool(pool, pool_count);
                return DB_OK;
            }
            strcpy(split_key, promoted);
        }
    }

    if (depth == 0) {
        BPlusNode *new_root = take_node(pool, &pool_count, 0);
        new_root->key_count = 1;
        strcpy(new_root->u.in.keys[0], split_key);
        new_root->u.in.children[0] = leaf;
        new_root->u.in.children[1] = right;
        tree->root = new_root;
    }
    free_node_pool(pool, pool_count);
    return DB_OK;
}

static void parent_remove_child(BPlusNode *parent, int child_index, int separator_index)
{
    int old_count = parent->key_count;
    memmove(parent->u.in.keys + separator_index,
        parent->u.in.keys + separator_index + 1,
        (size_t)(old_count - separator_index - 1) * sizeof(parent->u.in.keys[0]));
    memmove(parent->u.in.children + child_index,
        parent->u.in.children + child_index + 1,
        (size_t)(old_count - child_index) * sizeof(parent->u.in.children[0]));
    --parent->key_count;
    parent->u.in.children[parent->key_count + 1] = NULL;
}

DbStatus bptree_delete(BPlusTree *tree, const char *key)
{
    PathEntry path[256];
    BPlusNode *leaf;
    int depth;
    int index;
    int level;
    const int leaf_minimum = (LEAF_MAX_KEYS + 1) / 2;
    const int internal_minimum = (BT_ORDER + 1) / 2 - 1;

    if (tree == NULL || key == NULL || key[0] == '\0'
        || strlen(key) >= MAX_TAG_LENGTH) return DB_ERR_INVALID;
    leaf = find_leaf(tree, key, path, &depth);
    if (leaf == NULL) return DB_ERR_NOT_FOUND;
    index = leaf_lower_bound(leaf, key);
    if (index == leaf->key_count || strcmp(leaf->u.leaf.keys[index], key) != 0) {
        return DB_ERR_NOT_FOUND;
    }

    memmove(leaf->u.leaf.keys + index, leaf->u.leaf.keys + index + 1,
        (size_t)(leaf->key_count - index - 1) * sizeof(leaf->u.leaf.keys[0]));
    memmove(leaf->u.leaf.refs + index, leaf->u.leaf.refs + index + 1,
        (size_t)(leaf->key_count - index - 1) * sizeof(leaf->u.leaf.refs[0]));
    --leaf->key_count;
    if (depth == 0) {
        if (leaf->key_count == 0) {
            free(leaf);
            tree->root = NULL;
        }
        return DB_OK;
    }

    for (level = depth - 1; level >= 0; --level) {
        BPlusNode *parent = path[level].node;
        int child_index = path[level].child_index;
        BPlusNode *node = parent->u.in.children[child_index];
        int minimum = node->is_leaf ? leaf_minimum : internal_minimum;
        BPlusNode *left = child_index > 0 ? parent->u.in.children[child_index - 1] : NULL;
        BPlusNode *right = child_index < parent->key_count
            ? parent->u.in.children[child_index + 1] : NULL;

        if (node->key_count >= minimum) {
            if (node->is_leaf && child_index > 0 && node->key_count > 0) {
                strcpy(parent->u.in.keys[child_index - 1], node->u.leaf.keys[0]);
            }
            break;
        }

        if (left != NULL && left->key_count > minimum) {
            if (node->is_leaf) {
                memmove(node->u.leaf.keys + 1, node->u.leaf.keys,
                    (size_t)node->key_count * sizeof(node->u.leaf.keys[0]));
                memmove(node->u.leaf.refs + 1, node->u.leaf.refs,
                    (size_t)node->key_count * sizeof(node->u.leaf.refs[0]));
                strcpy(node->u.leaf.keys[0],
                    left->u.leaf.keys[left->key_count - 1]);
                node->u.leaf.refs[0] = left->u.leaf.refs[left->key_count - 1];
                --left->key_count;
                ++node->key_count;
                strcpy(parent->u.in.keys[child_index - 1], node->u.leaf.keys[0]);
            } else {
                int left_count = left->key_count;
                memmove(node->u.in.keys + 1, node->u.in.keys,
                    (size_t)node->key_count * sizeof(node->u.in.keys[0]));
                memmove(node->u.in.children + 1, node->u.in.children,
                    (size_t)(node->key_count + 1) * sizeof(node->u.in.children[0]));
                strcpy(node->u.in.keys[0], parent->u.in.keys[child_index - 1]);
                node->u.in.children[0] = left->u.in.children[left_count];
                strcpy(parent->u.in.keys[child_index - 1],
                    left->u.in.keys[left_count - 1]);
                --left->key_count;
                ++node->key_count;
            }
            if (tree->root != NULL) (void)refresh_separators(tree->root);
            return DB_OK;
        }
        if (right != NULL && right->key_count > minimum) {
            if (node->is_leaf) {
                strcpy(node->u.leaf.keys[node->key_count], right->u.leaf.keys[0]);
                node->u.leaf.refs[node->key_count] = right->u.leaf.refs[0];
                ++node->key_count;
                memmove(right->u.leaf.keys, right->u.leaf.keys + 1,
                    (size_t)(right->key_count - 1) * sizeof(right->u.leaf.keys[0]));
                memmove(right->u.leaf.refs, right->u.leaf.refs + 1,
                    (size_t)(right->key_count - 1) * sizeof(right->u.leaf.refs[0]));
                --right->key_count;
                strcpy(parent->u.in.keys[child_index], right->u.leaf.keys[0]);
            } else {
                int right_count = right->key_count;
                strcpy(node->u.in.keys[node->key_count],
                    parent->u.in.keys[child_index]);
                node->u.in.children[node->key_count + 1] = right->u.in.children[0];
                strcpy(parent->u.in.keys[child_index], right->u.in.keys[0]);
                memmove(right->u.in.keys, right->u.in.keys + 1,
                    (size_t)(right_count - 1) * sizeof(right->u.in.keys[0]));
                memmove(right->u.in.children, right->u.in.children + 1,
                    (size_t)right_count * sizeof(right->u.in.children[0]));
                --right->key_count;
                ++node->key_count;
            }
            if (tree->root != NULL) (void)refresh_separators(tree->root);
            return DB_OK;
        }

        if (left != NULL) {
            if (node->is_leaf) {
                memcpy(left->u.leaf.keys + left->key_count, node->u.leaf.keys,
                    (size_t)node->key_count * sizeof(node->u.leaf.keys[0]));
                memcpy(left->u.leaf.refs + left->key_count, node->u.leaf.refs,
                    (size_t)node->key_count * sizeof(node->u.leaf.refs[0]));
                left->key_count += node->key_count;
                left->u.leaf.next = node->u.leaf.next;
            } else {
                int offset = left->key_count;
                strcpy(left->u.in.keys[offset],
                    parent->u.in.keys[child_index - 1]);
                memcpy(left->u.in.keys + offset + 1, node->u.in.keys,
                    (size_t)node->key_count * sizeof(node->u.in.keys[0]));
                memcpy(left->u.in.children + offset + 1, node->u.in.children,
                    (size_t)(node->key_count + 1) * sizeof(node->u.in.children[0]));
                left->key_count += node->key_count + 1;
            }
            free(node);
            parent_remove_child(parent, child_index, child_index - 1);
        } else if (right != NULL) {
            if (node->is_leaf) {
                memcpy(node->u.leaf.keys + node->key_count, right->u.leaf.keys,
                    (size_t)right->key_count * sizeof(right->u.leaf.keys[0]));
                memcpy(node->u.leaf.refs + node->key_count, right->u.leaf.refs,
                    (size_t)right->key_count * sizeof(right->u.leaf.refs[0]));
                node->key_count += right->key_count;
                node->u.leaf.next = right->u.leaf.next;
            } else {
                int offset = node->key_count;
                strcpy(node->u.in.keys[offset], parent->u.in.keys[child_index]);
                memcpy(node->u.in.keys + offset + 1, right->u.in.keys,
                    (size_t)right->key_count * sizeof(right->u.in.keys[0]));
                memcpy(node->u.in.children + offset + 1, right->u.in.children,
                    (size_t)(right->key_count + 1) * sizeof(right->u.in.children[0]));
                node->key_count += right->key_count + 1;
            }
            free(right);
            parent_remove_child(parent, child_index + 1, child_index);
        }

        if (level == 0) {
            if (parent->key_count == 0) {
                tree->root = parent->u.in.children[0];
                free(parent);
            }
            if (tree->root != NULL) (void)refresh_separators(tree->root);
            return DB_OK;
        }
    }
    if (tree->root != NULL) (void)refresh_separators(tree->root);
    return DB_OK;
}

DbStatus bptree_iter_first(const BPlusTree *tree, BPIter *iterator)
{
    BPlusNode *node;
    if (tree == NULL || iterator == NULL) return DB_ERR_INVALID;
    node = tree->root;
    while (node != NULL && !node->is_leaf) node = node->u.in.children[0];
    iterator->leaf = node;
    iterator->index = 0;
    return DB_OK;
}

DbStatus bptree_iter_seek(const BPlusTree *tree, const char *low, BPIter *iterator)
{
    int depth;
    BPlusNode *leaf;
    if (tree == NULL || iterator == NULL || low == NULL || low[0] == '\0'
        || strlen(low) >= MAX_TAG_LENGTH) return DB_ERR_INVALID;
    leaf = find_leaf(tree, low, NULL, &depth);
    iterator->leaf = leaf;
    iterator->index = leaf == NULL ? 0 : leaf_lower_bound(leaf, low);
    return DB_OK;
}

int bptree_iter_next(BPIter *iterator, char key[MAX_TAG_LENGTH], RecordRef *ref)
{
    BPlusNode *leaf;
    if (iterator == NULL || key == NULL || ref == NULL) return 0;
    leaf = (BPlusNode *)iterator->leaf;
    while (leaf != NULL && iterator->index >= leaf->key_count) {
        leaf = leaf->u.leaf.next;
        iterator->leaf = leaf;
        iterator->index = 0;
    }
    if (leaf == NULL) return 0;
    strcpy(key, leaf->u.leaf.keys[iterator->index]);
    *ref = leaf->u.leaf.refs[iterator->index];
    ++iterator->index;
    return 1;
}

static DbStatus check_node(const BPlusNode *node, int is_root, int depth,
    CheckState *state, char minimum[MAX_TAG_LENGTH],
    char maximum[MAX_TAG_LENGTH])
{
    int i;

    if (node == NULL || depth > 256 || node->key_count < 0) return DB_ERR_INTERNAL;
    if (node->is_leaf) {
        if (node->key_count > LEAF_MAX_KEYS
            || (is_root ? node->key_count < 1 : node->key_count < (LEAF_MAX_KEYS + 1) / 2)) {
            return DB_ERR_INTERNAL;
        }
        for (i = 0; i < node->key_count; ++i) {
            if (node->u.leaf.keys[i][0] == '\0'
                || strlen(node->u.leaf.keys[i]) >= MAX_TAG_LENGTH
                || (i > 0 && strcmp(node->u.leaf.keys[i - 1],
                    node->u.leaf.keys[i]) >= 0)) {
                return DB_ERR_INTERNAL;
            }
        }
        if (state->leaf_depth < 0) state->leaf_depth = depth;
        else if (state->leaf_depth != depth) return DB_ERR_INTERNAL;
        if (state->previous_leaf != NULL) {
            int last = state->previous_leaf->key_count - 1;
            if (state->previous_leaf->u.leaf.next != node
                || strcmp(state->previous_leaf->u.leaf.keys[last],
                    node->u.leaf.keys[0]) >= 0) {
                return DB_ERR_INTERNAL;
            }
        }
        state->previous_leaf = (BPlusNode *)node;
        strcpy(minimum, node->u.leaf.keys[0]);
        strcpy(maximum, node->u.leaf.keys[node->key_count - 1]);
        return DB_OK;
    }

    if (node->key_count > INTERNAL_MAX_KEYS
        || (is_root ? node->key_count < 1
                    : node->key_count < (BT_ORDER + 1) / 2 - 1)) {
        return DB_ERR_INTERNAL;
    }
    for (i = 0; i < node->key_count; ++i) {
        if (node->u.in.keys[i][0] == '\0'
            || strlen(node->u.in.keys[i]) >= MAX_TAG_LENGTH
            || (i > 0 && strcmp(node->u.in.keys[i - 1],
                node->u.in.keys[i]) >= 0)) {
            return DB_ERR_INTERNAL;
        }
    }
    for (i = 0; i <= node->key_count; ++i) {
        char child_min[MAX_TAG_LENGTH];
        char child_max[MAX_TAG_LENGTH];
        DbStatus status = check_node(node->u.in.children[i], 0, depth + 1,
            state, child_min, child_max);
        if (status != DB_OK) return status;
        if ((i > 0 && strcmp(child_min, node->u.in.keys[i - 1]) != 0)
            || (i < node->key_count
                && strcmp(child_max, node->u.in.keys[i]) >= 0)) {
            return DB_ERR_INTERNAL;
        }
        if (i == 0) strcpy(minimum, child_min);
        if (i == node->key_count) strcpy(maximum, child_max);
    }
    return DB_OK;
}

DbStatus bptree_check(const BPlusTree *tree)
{
    CheckState state;
    char minimum[MAX_TAG_LENGTH];
    char maximum[MAX_TAG_LENGTH];
    DbStatus status;

    if (tree == NULL) return DB_ERR_INVALID;
    if (tree->root == NULL) return DB_OK;
    state.leaf_depth = -1;
    state.previous_leaf = NULL;
    status = check_node(tree->root, 1, 0, &state, minimum, maximum);
    if (status == DB_OK && state.previous_leaf->u.leaf.next != NULL) {
        return DB_ERR_INTERNAL;
    }
    return status;
}

void bptree_destroy(BPlusTree *tree)
{
    if (tree == NULL) return;
    node_destroy(tree->root);
    free(tree);
}
