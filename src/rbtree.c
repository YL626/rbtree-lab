#include "rbtree.h"
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

rbtree_t *rb_create(rb_value_free_fn value_free) {
    rbtree_t *t = rb_malloc(sizeof *t);
    if (t == NULL) return NULL;
    t->root = NULL;
    t->size = 0;
    t->value_free = value_free;
    return t;
}

void *rb_find(const rbtree_t *t, const char *key) {
    (void)t;
    (void)key;
    return NULL; /* real BST search lands in increment 2 */
}

size_t rb_size(const rbtree_t *t) {
    return t->size;
}

void rb_destroy(rbtree_t *t) {
    if (t == NULL) return;
    /* full non-recursive teardown lands in increment 5; for now the only
     * populated tree this stub will ever see is the empty one. */
    rb_free(t);
}
