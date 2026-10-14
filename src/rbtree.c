#include "rbtree.h"
#include "../tests/fault_alloc.h"
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* Mutation 2 pool entry points, defined in src/pool.c. Declared here rather
 * than in a src/pool.h: the spec's file map is fixed, and tests/test_rbtree.c
 * already declares these the same way under RBTREE_TEST_HOOKS. rb_pool_t is
 * left INCOMPLETE on purpose -- the tree logic only ever holds a pointer, so
 * no pool layout is visible here and none can drift out of step. */
typedef struct rb_pool rb_pool_t;
extern rb_pool_t *pool_create(size_t obj_size);
extern void      *pool_alloc(rb_pool_t *p);
extern void       pool_free(rb_pool_t *p, void *obj);
extern void       pool_destroy(rb_pool_t *p);

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
    /* NULL => ordinary rb_malloc node storage; non-NULL => node storage drawn
     * from this slab pool. NULL *is* the mode discriminator: no flag, no
     * second struct, and every mode-dependent decision in this file is made
     * inside node_alloc/node_release and rb_destroy's teardown branch. */
    rb_pool_t        *pool;
};

static bool is_red(const struct rb_node *n)  { return n != NULL && n->color == RED; }
static bool is_black(const struct rb_node *n) { return !is_red(n); }

/* The single point where node storage is obtained and returned. Centralizing
 * them is what keeps rb_insert/rb_delete free of mode-specific branches: both
 * call these and neither knows which allocator is underneath.
 *
 * Only NODE storage is pooled. Key copies are variable-length and stay on the
 * rb_malloc/rb_free seam -- a fixed-stride slot pool cannot hold them.
 *
 * node_release is deliberately NOT used by rb_destroy's whole-tree teardown:
 * see the comment there. It exists for rb_delete and for rb_insert's rollback
 * when the key copy fails after a slot has already been taken. */
static struct rb_node *node_alloc(rbtree_t *t) {
    if (t->pool != NULL) return pool_alloc(t->pool);
    return rb_malloc(sizeof(struct rb_node));
}

static void node_release(rbtree_t *t, struct rb_node *n) {
    if (t->pool != NULL) pool_free(t->pool, n);
    else                 rb_free(n);
}

rbtree_t *rb_create(rb_value_free_fn value_free) {
    rbtree_t *t = rb_malloc(sizeof *t);
    if (t == NULL) return NULL;
    t->root = NULL;
    t->size = 0;
    t->value_free = value_free;
    t->pool = NULL;
    return t;
}

/* Mutation 2: a tree whose NODES come from an internal slab pool. Everything
 * else -- the tree handle itself, and every key copy -- still goes through
 * rb_malloc, since neither is a fixed-size object the pool could hold.
 *
 * Two fallible steps, and the order is chosen so there is exactly one thing
 * to unwind: the handle first (nothing held if it fails), then the pool
 * (release the handle if it fails). pool_create is itself all-or-nothing --
 * it validates the geometry before its single rb_malloc -- so there is never
 * a half-built pool to dismantle, and no goto-cleanup ladder is warranted.
 *
 * rb_free(t) rather than rb_destroy(t) on the pool-create failure path: t is
 * an empty tree whose pool field rb_create has already set to NULL, so
 * rb_destroy would be correct, but rb_free says plainly that nothing has been
 * built yet and cannot be misread as teardown of a populated tree. */
rbtree_t *rb_create_pooled(rb_value_free_fn value_free) {
    rbtree_t *t = rb_create(value_free);
    if (t == NULL) return NULL;

    t->pool = pool_create(sizeof(struct rb_node));
    if (t->pool == NULL) {
        rb_free(t);
        return NULL;
    }
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

    struct rb_node *n = node_alloc(t);
    if (n == NULL) return -1;

    size_t keylen = strlen(key) + 1;
    n->key = rb_malloc(keylen);
    if (n->key == NULL) {
        /* Rollback: the slot goes straight back, so a pooled tree's
         * live + free_objs == slabs * objs_per_slab still holds across the
         * failure. n->value was never assigned and the caller still owns the
         * value it passed in. */
        node_release(t, n);
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

static struct rb_node *find_node(const rbtree_t *t, const char *key) {
    struct rb_node *cur = t->root;
    while (cur != NULL) {
        int cmp = strcmp(key, cur->key);
        if (cmp == 0) return cur;
        cur = (cmp < 0) ? cur->left : cur->right;
    }
    return NULL;
}

static struct rb_node *tree_min(struct rb_node *n) {
    while (n->left != NULL) n = n->left;
    return n;
}

/* Puts v in u's slot (u->parent, or t->root) -- the only place delete
 * touches t->root or reparents a node, mirroring the rotations. */
static void transplant(rbtree_t *t, struct rb_node *u, struct rb_node *v) {
    if (v != NULL) v->parent = u->parent;
    if (u->parent == NULL)         t->root = v;
    else if (u == u->parent->left) u->parent->left = v;
    else                            u->parent->right = v;
}

/* x may be NULL (NIL); since NULL can't carry its own parent pointer,
 * (parent, x) is threaded through the loop as an explicit pair instead
 * of ever reading x->parent -- see NOTES.md's devlog on why this tree
 * has no shared sentinel. */
static void delete_fixup(rbtree_t *t, struct rb_node *parent, struct rb_node *x) {
    while (parent != NULL && is_black(x)) {
        if (x == parent->left) {
            struct rb_node *sib = parent->right;
            if (is_red(sib)) {                     /* Case 5: sibling red */
                sib->color = BLACK;
                parent->color = RED;
                rotate_left(t, parent);
                sib = parent->right;
            }
            if (is_black(sib->left) && is_black(sib->right)) { /* Case 3: both nephews black */
                sib->color = RED;
                x = parent;
                parent = x->parent;
            } else {
                if (is_black(sib->right)) {         /* near red / far black: convert first */
                    if (sib->left != NULL) sib->left->color = BLACK;
                    sib->color = RED;
                    rotate_right(t, sib);
                    sib = parent->right;
                }
                sib->color = parent->color;         /* Case 4: far nephew red, terminal */
                parent->color = BLACK;
                if (sib->right != NULL) sib->right->color = BLACK;
                rotate_left(t, parent);
                return;
            }
        } else {
            /* exact mirror: left/right swapped, rotate_right/rotate_left swapped */
            struct rb_node *sib = parent->left;
            if (is_red(sib)) {
                sib->color = BLACK;
                parent->color = RED;
                rotate_right(t, parent);
                sib = parent->left;
            }
            if (is_black(sib->left) && is_black(sib->right)) {
                sib->color = RED;
                x = parent;
                parent = x->parent;
            } else {
                if (is_black(sib->left)) {
                    if (sib->right != NULL) sib->right->color = BLACK;
                    sib->color = RED;
                    rotate_left(t, sib);
                    sib = parent->left;
                }
                sib->color = parent->color;
                parent->color = BLACK;
                if (sib->left != NULL) sib->left->color = BLACK;
                rotate_right(t, parent);
                return;
            }
        }
    }
    if (x != NULL) x->color = BLACK;
}

int rb_delete(rbtree_t *t, const char *key) {
    struct rb_node *z = find_node(t, key);
    if (z == NULL) return -1;

    struct rb_node *n;
    bool free_own_payload = true;

    if (z->left != NULL && z->right != NULL) {
        struct rb_node *y = tree_min(z->right);
        rb_free(z->key);
        if (t->value_free) t->value_free(z->value);
        z->key = y->key;
        z->value = y->value;
        n = y;
        free_own_payload = false;
    } else {
        n = z;
    }

    struct rb_node *child = (n->left != NULL) ? n->left : n->right;
    bool n_was_black = is_black(n);
    transplant(t, n, child);

    if (n_was_black) {
        if (child != NULL) {
            child->color = BLACK;             /* STEP1: absorbs the debt locally */
        } else {
            delete_fixup(t, n->parent, NULL); /* STEP2: only after transplant has run */
        }
    }

    if (free_own_payload) {
        rb_free(n->key);
        if (t->value_free) t->value_free(n->value);
    }
    node_release(t, n);
    t->size--;
    return 0;
}

size_t rb_size(const rbtree_t *t) {
    return t->size;
}

/* In-order walk over parent pointers: no recursion, no allocation, two
 * pointers of state, and the tree is never written. Each edge is descended
 * once and climbed once, so the whole walk is O(n).
 *
 * Callback contract. fn may make read-only calls on this same tree
 * (rb_find, rb_size, rb_validate, a nested rb_foreach) and may do anything to
 * other trees. fn must not call rb_insert, rb_delete or rb_destroy on the tree
 * being traversed: the successor is read from the current node after fn
 * returns, and a rotation, a freed node or (for a two-children delete) a
 * freed successor would leave that read undefined. No mutating call is
 * supported, not even on the node just visited. Because the walk never
 * modifies the tree and holds no resources, a callback that never returns
 * (exit, longjmp) leaves the tree valid and fully usable. */
void rb_foreach(const rbtree_t *t,
                void (*fn)(const char *key, void *value, void *ctx), void *ctx) {
    const struct rb_node *cur = t->root;
    if (cur == NULL) return;
    while (cur->left != NULL) cur = cur->left;
    /* Invariant: exactly the keys < cur->key have been passed to fn, each once. */
    while (cur != NULL) {
        fn(cur->key, cur->value, ctx);
        if (cur->right != NULL) {
            /* Successor is the leftmost node of the right subtree. */
            cur = cur->right;
            while (cur->left != NULL) cur = cur->left;
        } else {
            /* Climb past every ancestor we reached from its right side; the
             * first one reached from the left is next, NULL means done. */
            const struct rb_node *child;
            do {
                child = cur;
                cur = cur->parent;
            } while (cur != NULL && child == cur->right);
        }
    }
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

/* Teardown. The right-spine "Reach" walk is unchanged; the only mode-dependent
 * part is who reclaims node storage.
 *
 * Ordinary tree: each node is rb_free'd as the walk drops it.
 *
 * Pooled tree: nodes are NOT individually pool_free'd. Whole-tree destruction
 * is exactly the case pool_destroy is for -- it releases every slab whether
 * its slots were live, free-listed, or never carved -- so per-node pool_free
 * would cost an O(n) pass of poisoning and free-list churn to hand slots back
 * to a pool that is about to cease existing. What the walk is still required
 * for is the payloads: keys and values live OUTSIDE the slabs and are not
 * reclaimed by pool_destroy, so every key and owned value must be freed while
 * its node is still readable.
 *
 * Hence the ordering, which is load-bearing in both modes:
 *   1. capture `next` before anything is released,
 *   2. free the key,
 *   3. invoke value_free,
 *   4. (ordinary only) rb_free the node,
 * then after the walk, pool_destroy and finally the handle. Step 1 matters
 * more under the pool than without it: a pooled node is still live slab memory
 * after teardown passes over it, so a stale read would be invisible to ASan
 * rather than a reported use-after-free. */
void rb_destroy(rbtree_t *t) {
    if (t == NULL) return;
    struct rb_node *cur = t->root;
    /* Invariant: cur reaches N unfreed nodes; 2*N-S decreases (S = right-spine length). */
    while (cur != NULL) {
        struct rb_node *next;
        if (cur->left == NULL) {
            next = cur->right;
            rb_free(cur->key);
            if (t->value_free) t->value_free(cur->value);
            if (t->pool == NULL) rb_free(cur); /* pooled: reclaimed wholesale below */
        } else {
            struct rb_node *left = cur->left;
            cur->left = left->right;
            left->right = cur;
            next = left; /* re-enter the loop rooted at the promoted node */
        }
        cur = next;
    }
    /* Last, and only after the walk: every node it traversed physically lives
     * inside a slab this releases. */
    if (t->pool != NULL) pool_destroy(t->pool);
    rb_free(t);
}

#ifdef RBTREE_TEST_HOOKS
/* Test-only hooks for exercising rb_validate's negative paths (invariants
 * 1-5). Compiled in only when RBTREE_TEST_HOOKS is defined (test_rbtree's
 * build only, per the Makefile) so production/grading builds that compile
 * this file without the macro never see these symbols at all. Each hook
 * mutates only existing struct fields on nodes the tree already owns --
 * no allocation, no freeing -- so rb_destroy's pointer-only teardown walk
 * (left/right only; it never reads color, key contents, or t->size) stays
 * safe to call afterward regardless of which invariant was broken.
 *
 * rb_validate checks invariants in order (1: root black, 2: no red-red,
 * 3: black-height, 4: ordering, 5: size) and returns on the first failure,
 * which is what lets each hook below isolate its target invariant's return
 * code even when a mutation has a side effect on a later check. */

/* Invariant 1: flip the root red. Root-color is checked first and
 * unconditionally, so this isolates invariant 1 regardless of anything
 * else about the tree. */
void rb_test_force_root_red(rbtree_t *t) {
    if (t != NULL && t->root != NULL) t->root->color = RED;
}

/* Invariant 2: find a RED node with a real (non-NULL) BLACK child and flip
 * that child RED, producing a red-red pair. Returns 1 if found and
 * corrupted, 0 if the fixture didn't have the needed shape. */
static struct rb_node *find_red_with_black_child(struct rb_node *n) {
    if (n == NULL) return NULL;
    if (n->color == RED &&
        ((n->left != NULL && n->left->color == BLACK) ||
         (n->right != NULL && n->right->color == BLACK))) {
        return n;
    }
    struct rb_node *found = find_red_with_black_child(n->left);
    if (found != NULL) return found;
    return find_red_with_black_child(n->right);
}

int rb_test_force_red_red(rbtree_t *t) {
    if (t == NULL) return 0;
    struct rb_node *p = find_red_with_black_child(t->root);
    if (p == NULL) return 0;
    struct rb_node *child = (p->left != NULL && p->left->color == BLACK)
                             ? p->left : p->right;
    child->color = RED;
    return 1;
}

/* Invariant 3: find a non-root BLACK node whose parent is BLACK and whose
 * own children are both BLACK-or-NIL, then flip it RED. That perturbs only
 * that node's subtree's black count -- it can't create a red-red pair
 * (parent stays black, its own children stay black) and it doesn't touch
 * the root, so it isolates invariant 3 rather than tripping 1 or 2 first.
 * Returns 1 if found and corrupted, 0 otherwise. */
static struct rb_node *find_isolated_black_flip_target(struct rb_node *n) {
    if (n == NULL) return NULL;
    if (n->parent != NULL && n->color == BLACK && is_black(n->parent) &&
        is_black(n->left) && is_black(n->right)) {
        return n;
    }
    struct rb_node *found = find_isolated_black_flip_target(n->left);
    if (found != NULL) return found;
    return find_isolated_black_flip_target(n->right);
}

int rb_test_break_black_height(rbtree_t *t) {
    if (t == NULL) return 0;
    struct rb_node *n = find_isolated_black_flip_target(t->root);
    if (n == NULL) return 0;
    n->color = RED;
    return 1;
}

/* Invariant 4: swap the key pointers of two existing nodes (located by
 * their current key values), which breaks in-order strictly-increasing
 * order without touching allocation, color, or structure -- each string is
 * still owned by exactly one node afterward, just a different one. Returns
 * 1 if both keys were found and swapped, 0 otherwise. */
static struct rb_node *find_by_key(struct rb_node *n, const char *key) {
    while (n != NULL) {
        int cmp = strcmp(key, n->key);
        if (cmp == 0) return n;
        n = (cmp < 0) ? n->left : n->right;
    }
    return NULL;
}

int rb_test_swap_keys(rbtree_t *t, const char *key1, const char *key2) {
    if (t == NULL) return 0;
    struct rb_node *n1 = find_by_key(t->root, key1);
    struct rb_node *n2 = find_by_key(t->root, key2);
    if (n1 == NULL || n2 == NULL || n1 == n2) return 0;
    char *tmp = n1->key;
    n1->key = n2->key;
    n2->key = tmp;
    return 1;
}

/* Invariant 5: directly desync the size counter from the real node count,
 * touching nothing else. Works on any tree, even empty. */
void rb_test_bump_size(rbtree_t *t) {
    if (t != NULL) t->size += 1;
}

/* Mutation 3: rb_insert/rb_delete keep height O(log n), so no public-API
 * sequence yields a deep tree, and a recursive teardown would pass every
 * balanced test. This reshapes an existing tree into a single spine (all
 * left children if `left`, else all right children) using the real
 * rotations, so parent pointers, t->root, t->size, in-order sequence and
 * ownership are all preserved; only colors become meaningless (rb_validate
 * will report invariant 1-3 failures afterward). Iterative, O(1) space, no
 * allocation. Verified separately by rb_test_spine_len, so the reshape never
 * vouches for itself. */
void rb_test_make_spine(rbtree_t *t, bool left) {
    if (t == NULL) return;
    struct rb_node *cur = t->root;
    /* Invariant: every node above cur has no child on the off-spine side. */
    while (cur != NULL) {
        struct rb_node *off = left ? cur->right : cur->left;
        if (off != NULL) {
            if (left) rotate_left(t, cur);
            else      rotate_right(t, cur);
            cur = off; /* off now occupies cur's old position */
        } else {
            cur = left ? cur->left : cur->right;
        }
    }
}

/* Length of the path from the root following only left (or only right)
 * links. Equals rb_size exactly when the tree is a single spine. */
size_t rb_test_spine_len(const rbtree_t *t, bool left) {
    size_t len = 0;
    if (t == NULL) return 0;
    for (const struct rb_node *n = t->root; n != NULL; n = left ? n->left : n->right) len++;
    return len;
}

/* Structural check rb_validate does not make: every child's parent pointer
 * names its actual parent, the root's is NULL, the in-order sequence is
 * strictly increasing, and the node count equals t->size. Iterative in-order
 * walk over parent pointers, O(1) space, so it is safe on a degenerate spine.
 * An upward step only ever follows a parent link that was verified on the way
 * down, so the walk never trusts a pointer it is checking. Returns true when
 * all of it holds. */
bool rb_test_links_ok(const rbtree_t *t) {
    if (t == NULL) return false;
    const struct rb_node *cur = t->root, *prev = NULL;
    size_t count = 0;
    if (cur == NULL) return t->size == 0;
    if (cur->parent != NULL) return false;
    while (cur->left != NULL) {
        if (cur->left->parent != cur) return false;
        cur = cur->left;
    }
    /* Invariant: cur is the in-order successor of prev, and every edge on
     * the path root..cur has had its child->parent link verified. */
    while (cur != NULL) {
        if (prev != NULL && strcmp(prev->key, cur->key) >= 0) return false;
        if (++count > t->size) return false; /* also bounds a corrupted cycle */
        prev = cur;
        if (cur->right != NULL) {
            if (cur->right->parent != cur) return false;
            cur = cur->right;
            while (cur->left != NULL) {
                if (cur->left->parent != cur) return false;
                cur = cur->left;
            }
        } else {
            while (cur->parent != NULL && cur == cur->parent->right) cur = cur->parent;
            cur = cur->parent;
        }
    }
    return count == t->size;
}
#endif
