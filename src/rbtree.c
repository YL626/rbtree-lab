#include "rbtree.h"
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

typedef enum { RED, BLACK } rb_color_t;

struct rb_node {
    char           *key;
    void           *value;
    struct rb_node *parent;
    struct rb_node *left;
    struct rb_node *right;
    rb_color_t      color;
};

struct rbtree {
    struct rb_node   *root;
    size_t            size;
    rb_value_free_fn  value_free;
};

static void *rb_malloc(size_t n) { return malloc(n); }
static void  rb_free(void *p)    { free(p); }

static bool is_red(const struct rb_node *n)  { return n != NULL && n->color == RED; }
static bool is_black(const struct rb_node *n) { return !is_red(n); }

rbtree_t *rb_create(rb_value_free_fn value_free) {
    rbtree_t *t = rb_malloc(sizeof *t);
    if (t == NULL) return NULL;
    t->root = NULL;
    t->size = 0;
    t->value_free = value_free;
    return t;
}

void *rb_find(const rbtree_t *t, const char *key) {
    struct rb_node *cur = t->root;
    while (cur != NULL) {
        int cmp = strcmp(key, cur->key);
        if (cmp == 0) return cur->value;
        cur = (cmp < 0) ? cur->left : cur->right;
    }
    return NULL;
}

/* Standard BST rotation, extended to keep parent pointers and t->root
 * correct — rotations own root maintenance, not rb_validate. */
static void rotate_left(rbtree_t *t, struct rb_node *x) {
    struct rb_node *y = x->right;
    x->right = y->left;
    if (y->left != NULL) y->left->parent = x;
    y->parent = x->parent;
    if (x->parent == NULL)        t->root = y;
    else if (x == x->parent->left) x->parent->left = y;
    else                            x->parent->right = y;
    y->left = x;
    x->parent = y;
}

static void rotate_right(rbtree_t *t, struct rb_node *x) {
    struct rb_node *y = x->left;
    x->left = y->right;
    if (y->right != NULL) y->right->parent = x;
    y->parent = x->parent;
    if (x->parent == NULL)         t->root = y;
    else if (x == x->parent->right) x->parent->right = y;
    else                             x->parent->left = y;
    y->right = x;
    x->parent = y;
}

static void insert_fixup(rbtree_t *t, struct rb_node *z) {
    /* Invariant: z is red. Loop while z's parent is also red (the only
     * possible violation after a plain BST insert of a red node). */
    while (is_red(z->parent)) {
        struct rb_node *parent = z->parent;
        struct rb_node *grandparent = parent->parent;
        if (parent == grandparent->left) {
            struct rb_node *uncle = grandparent->right;
            if (is_red(uncle)) {
                parent->color = BLACK;
                uncle->color = BLACK;
                grandparent->color = RED;
                z = grandparent;
            } else {
                if (z == parent->right) {
                    z = parent;
                    rotate_left(t, z);
                    parent = z->parent;
                }
                parent->color = BLACK;
                grandparent->color = RED;
                rotate_right(t, grandparent);
            }
        } else {
            struct rb_node *uncle = grandparent->left;
            if (is_red(uncle)) {
                parent->color = BLACK;
                uncle->color = BLACK;
                grandparent->color = RED;
                z = grandparent;
            } else {
                if (z == parent->left) {
                    z = parent;
                    rotate_right(t, z);
                    parent = z->parent;
                }
                parent->color = BLACK;
                grandparent->color = RED;
                rotate_left(t, grandparent);
            }
        }
    }
    t->root->color = BLACK;
}

int rb_insert(rbtree_t *t, const char *key, void *value) {
    struct rb_node *parent = NULL;
    struct rb_node *cur = t->root;
    int cmp = 0;
    while (cur != NULL) {
        cmp = strcmp(key, cur->key);
        if (cmp == 0) {
            if (t->value_free) t->value_free(cur->value);
            cur->value = value;
            return 0;
        }
        parent = cur;
        cur = (cmp < 0) ? cur->left : cur->right;
    }

    struct rb_node *n = rb_malloc(sizeof *n);
    if (n == NULL) return -1;

    size_t keylen = strlen(key) + 1;
    n->key = rb_malloc(keylen);
    if (n->key == NULL) {
        rb_free(n);
        return -1;
    }
    memcpy(n->key, key, keylen);

    n->value = value;
    n->color = RED;
    n->left = n->right = NULL;
    n->parent = parent;

    if (parent == NULL)      t->root = n;
    else if (cmp < 0)        parent->left = n;
    else                     parent->right = n;

    insert_fixup(t, n);
    t->size++;
    return 0;
}

size_t rb_size(const rbtree_t *t) {
    return t->size;
}

static void foreach_inorder(const struct rb_node *n,
                             void (*fn)(const char *, void *, void *), void *ctx) {
    if (n == NULL) return;
    foreach_inorder(n->left, fn, ctx);
    fn(n->key, n->value, ctx);
    foreach_inorder(n->right, fn, ctx);
}

void rb_foreach(const rbtree_t *t,
                void (*fn)(const char *key, void *value, void *ctx), void *ctx) {
    foreach_inorder(t->root, fn, ctx);
}

struct validate_state {
    const char *prev_key;
    bool        order_ok;
    size_t      count;
};

static void validate_walk(const struct rb_node *n, struct validate_state *st) {
    if (n == NULL) return;
    validate_walk(n->left, st);
    if (st->prev_key != NULL && strcmp(st->prev_key, n->key) >= 0) {
        st->order_ok = false;
    }
    st->prev_key = n->key;
    st->count++;
    validate_walk(n->right, st);
}

static bool has_red_red(const struct rb_node *n) {
    if (n == NULL) return false;
    if (is_red(n) && (is_red(n->left) || is_red(n->right))) return true;
    return has_red_red(n->left) || has_red_red(n->right);
}

/* Returns the subtree's black-height, or -1 if any root-to-NIL path within it
 * has already been found to disagree with another. NULL (NIL) contributes a
 * black-height of 1 by convention; a black node adds 1 on top of its children's
 * (equal) black-height. */
static int black_height(const struct rb_node *n) {
    if (n == NULL) return 1;
    int lh = black_height(n->left);
    if (lh == -1) return -1;
    int rh = black_height(n->right);
    if (rh == -1) return -1;
    if (lh != rh) return -1;
    return lh + (is_black(n) ? 1 : 0);
}

int rb_validate(const rbtree_t *t) {
    if (is_red(t->root)) {
        fprintf(stderr, "rb_validate: invariant 1 violated (root is red)\n");
        return 1;
    }
    if (has_red_red(t->root)) {
        fprintf(stderr, "rb_validate: invariant 2 violated "
                         "(a red node has a red child)\n");
        return 2;
    }
    if (black_height(t->root) == -1) {
        fprintf(stderr, "rb_validate: invariant 3 violated "
                         "(root-to-NIL paths have differing black-height)\n");
        return 3;
    }

    struct validate_state st = { .prev_key = NULL, .order_ok = true, .count = 0 };
    validate_walk(t->root, &st);

    if (!st.order_ok) {
        fprintf(stderr, "rb_validate: invariant 4 violated "
                         "(in-order keys not strictly increasing under strcmp)\n");
        return 4;
    }
    if (st.count != t->size) {
        fprintf(stderr, "rb_validate: invariant 5 violated "
                         "(rb_size=%zu, actual node count=%zu)\n", t->size, st.count);
        return 5;
    }
    return 0;
}

void rb_destroy(rbtree_t *t) {
    if (t == NULL) return;
    struct rb_node *cur = t->root;
    while (cur != NULL) {
        struct rb_node *next;
        if (cur->left == NULL) {
            next = cur->right;
            rb_free(cur->key);
            if (t->value_free) t->value_free(cur->value);
            rb_free(cur);
        } else {
            struct rb_node *left = cur->left;
            cur->left = left->right;
            left->right = cur;
            next = left; /* re-enter the loop rooted at the promoted node */
        }
        cur = next;
    }
    rb_free(t);
}
