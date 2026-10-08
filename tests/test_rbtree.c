#include "rbtree.h"
#include "fault_alloc.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef RBTREE_TEST_HOOKS
/* Defined in src/rbtree.c, only compiled in under RBTREE_TEST_HOOKS.
 * Not part of the frozen public API in rbtree.h -- these exist solely to
 * let tests manufacture invalid trees so rb_validate's negative paths can
 * be exercised (see NOTES.md/PROMPTLOG.md for why the fuzzer alone can't:
 * rb_insert/rb_delete never produce an invalid tree by construction). */
extern void rb_test_force_root_red(rbtree_t *t);
extern int  rb_test_force_red_red(rbtree_t *t);
extern int  rb_test_break_black_height(rbtree_t *t);
extern int  rb_test_swap_keys(rbtree_t *t, const char *key1, const char *key2);
extern void rb_test_bump_size(rbtree_t *t);

/* Defined in src/pool.c (Mutation 2 slot geometry). Internal to the pool:
 * absent from the frozen rbtree.h and deliberately given no header of its
 * own, so it is declared here like the rb_test_* hooks above. It has external
 * linkage in every build -- that is what keeps the production build
 * warning-clean while no pool_create calls it yet -- but only this test
 * declares it. */
extern bool rb_pool_geometry(size_t obj_size, size_t *stride, size_t *slot0_offset,
                             size_t *objs_per_slab, size_t *slack);

/* Also src/pool.c (Mutation 2 pool lifecycle, slice A). Declared here for the
 * same reasons as the geometry helper above, with one addition: rb_pool_t is
 * left INCOMPLETE. The tests need only the pointer type, so no struct layout
 * is duplicated across the file boundary and none can drift out of step.
 * pool_alloc and pool_free do not exist yet and are absent on purpose. */
typedef struct rb_pool rb_pool_t;
extern rb_pool_t *pool_create(size_t obj_size);
extern void      *pool_alloc(rb_pool_t *p);
extern void       pool_free(rb_pool_t *p, void *obj);
extern void       pool_stats(const rb_pool_t *p, size_t *slabs, size_t *live,
                             size_t *free_objs);
extern void       pool_destroy(rb_pool_t *p);
#endif

static int failures = 0;

static void check(int cond, const char *desc) {
    if (cond) { printf("PASS: %s\n", desc); }
    else      { printf("FAIL: %s\n", desc); failures++; }
}

static void test_create_destroy_empty(void) {
    rbtree_t *t = rb_create(NULL);
    check(t != NULL, "rb_create returns non-NULL");
    check(rb_size(t) == 0, "fresh tree has size 0");
    check(rb_find(t, "anything") == NULL, "rb_find on empty tree returns NULL");
    rb_destroy(t);
}

static void test_destroy_null_is_safe(void) {
    rb_destroy(NULL); /* must not crash; asan/valgrind judge this, not an assertion */
    check(1, "rb_destroy(NULL) did not crash");
}

static void test_insert_and_find_single(void) {
    rbtree_t *t = rb_create(NULL);
    int value = 42;
    check(rb_insert(t, "apple", &value) == 0, "insert single key succeeds");
    check(rb_find(t, "apple") == &value, "find returns the inserted value");
    check(rb_size(t) == 1, "size is 1 after one insert");
    rb_destroy(t);
}

static void test_insert_and_find_multiple(void) {
    rbtree_t *t = rb_create(NULL);
    int a = 1, b = 2, c = 3;
    check(rb_insert(t, "banana", &a) == 0, "insert banana");
    check(rb_insert(t, "apple", &b) == 0, "insert apple");
    check(rb_insert(t, "cherry", &c) == 0, "insert cherry");
    check(rb_find(t, "banana") == &a, "find banana after multiple inserts");
    check(rb_find(t, "apple") == &b, "find apple after multiple inserts");
    check(rb_find(t, "cherry") == &c, "find cherry after multiple inserts");
    check(rb_size(t) == 3, "size is 3 after three distinct inserts");
    rb_destroy(t);
}

static void test_find_missing_key_returns_null(void) {
    rbtree_t *t = rb_create(NULL);
    int value = 7;
    check(rb_insert(t, "apple", &value) == 0, "insert apple");
    check(rb_find(t, "banana") == NULL, "find a never-inserted key returns NULL");
    rb_destroy(t);
}

static int free_count = 0;

static void counting_free(void *value) {
    free(value);
    free_count++;
}

static int *make_int(int v) {
    int *p = malloc(sizeof *p);
    check(p != NULL, "test fixture malloc succeeded"); /* test-only; not graded like src/ */
    *p = v;
    return p;
}

static void test_overwrite_frees_old_value(void) {
    free_count = 0;
    rbtree_t *t = rb_create(counting_free);
    int *first  = make_int(1);
    int *second = make_int(2);
    check(rb_insert(t, "apple", first) == 0, "insert apple with first value");
    check(rb_insert(t, "apple", second) == 0, "overwrite apple with second value");
    check(free_count == 1, "value_free called exactly once on overwrite");
    check(*(int *)rb_find(t, "apple") == 2, "find returns the new value after overwrite");
    check(rb_size(t) == 1, "size unchanged after overwrite (no new node)");
    rb_destroy(t);
    check(free_count == 2, "value_free called again for the remaining node on destroy");
}

static void test_overwrite_with_null_value_free_does_not_crash(void) {
    rbtree_t *t = rb_create(NULL);
    int a = 1, b = 2;
    check(rb_insert(t, "apple", &a) == 0, "insert apple");
    check(rb_insert(t, "apple", &b) == 0, "overwrite apple with value_free NULL");
    check(rb_find(t, "apple") == &b, "find returns new value after NULL-destructor overwrite");
    check(rb_size(t) == 1, "size unchanged after overwrite with NULL destructor");
    rb_destroy(t);
}

#define TEST_MAX_KEYS 40

struct order_ctx {
    const char *keys[TEST_MAX_KEYS];
    size_t      count;
};

static void collect_key(const char *key, void *value, void *ctx_ptr) {
    (void)value;
    struct order_ctx *ctx = ctx_ptr;
    ctx->keys[ctx->count++] = key;
}

static void test_foreach_visits_in_order(void) {
    rbtree_t *t = rb_create(NULL);
    const char *keys[5] = {"cherry", "apple", "elderberry", "banana", "date"};
    int values[5] = {0};
    for (int i = 0; i < 5; i++) {
        check(rb_insert(t, keys[i], &values[i]) == 0, "insert during foreach setup");
    }
    struct order_ctx ctx = { .count = 0 };
    rb_foreach(t, collect_key, &ctx);
    check(ctx.count == 5, "foreach visited every node exactly once");
    int sorted = 1;
    for (size_t i = 1; i < ctx.count; i++) {
        if (strcmp(ctx.keys[i - 1], ctx.keys[i]) >= 0) sorted = 0;
    }
    check(sorted, "foreach visits keys in strictly increasing strcmp order");
    rb_destroy(t);
}

static void test_validate_on_ascending_insertion_order(void) {
    rbtree_t *t = rb_create(NULL);
    const char *keys[5] = {"a", "b", "c", "d", "e"};
    int values[5] = {0};
    for (int i = 0; i < 5; i++) check(rb_insert(t, keys[i], &values[i]) == 0, "insert ascending");
    check(rb_validate(t) == 0, "validate passes on ascending-inserted tree");
    rb_destroy(t);
}

static void test_validate_on_descending_insertion_order(void) {
    rbtree_t *t = rb_create(NULL);
    const char *keys[5] = {"e", "d", "c", "b", "a"};
    int values[5] = {0};
    for (int i = 0; i < 5; i++) check(rb_insert(t, keys[i], &values[i]) == 0, "insert descending");
    check(rb_validate(t) == 0, "validate passes on descending-inserted tree");
    rb_destroy(t);
}

static void test_validate_on_scrambled_insertion_order(void) {
    rbtree_t *t = rb_create(NULL);
    const char *keys[5] = {"cherry", "apple", "elderberry", "banana", "date"};
    int values[5] = {0};
    for (int i = 0; i < 5; i++) check(rb_insert(t, keys[i], &values[i]) == 0, "insert scrambled");
    check(rb_validate(t) == 0, "validate passes on scrambled-insertion-order tree");
    rb_destroy(t);
}

static void test_validate_on_empty_tree(void) {
    rbtree_t *t = rb_create(NULL);
    check(rb_validate(t) == 0, "validate passes on empty tree");
    rb_destroy(t);
}

#ifdef RBTREE_TEST_HOOKS
static void test_validate_detects_red_root(void) {
    rbtree_t *t = rb_create(NULL);
    int a = 1, b = 2, c = 3;
    check(rb_insert(t, "b", &a) == 0, "insert b for red-root corruption fixture");
    check(rb_insert(t, "a", &b) == 0, "insert a for red-root corruption fixture");
    check(rb_insert(t, "c", &c) == 0, "insert c for red-root corruption fixture");
    check(rb_validate(t) == 0, "validate passes before corruption");
    rb_test_force_root_red(t);
    check(rb_validate(t) == 1, "validate detects invariant 1 (root is red)");
    rb_destroy(t);
}

/* Insertion order for the invariant 2/3/4 fixtures below: verified (via a
 * throwaway scratch probe against the real compiled rb_insert, never a
 * reimplementation -- see PROMPTLOG.md) to produce node 'd' RED with real
 * BLACK children 'c'/'f' (invariant 2's fixture) and node 'a' BLACK with
 * BLACK parent 'b' and NIL children (invariant 3's fixture). */
static rbtree_t *build_seven_key_fixture(void) {
    rbtree_t *t = rb_create(NULL);
    static int dummy;
    const char *keys[] = {"b", "a", "c", "e", "d", "f", "g"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        check(rb_insert(t, keys[i], &dummy) == 0, "insert key for 7-key fixture");
    }
    return t;
}

static void test_validate_detects_red_red(void) {
    rbtree_t *t = build_seven_key_fixture();
    check(rb_validate(t) == 0, "validate passes before corruption");
    check(rb_test_force_red_red(t) == 1,
          "corruption hook found a red node with a black child");
    check(rb_validate(t) == 2, "validate detects invariant 2 (red node has a red child)");
    rb_destroy(t);
}

static void test_validate_detects_black_height_mismatch(void) {
    rbtree_t *t = build_seven_key_fixture();
    check(rb_validate(t) == 0, "validate passes before corruption");
    check(rb_test_break_black_height(t) == 1,
          "corruption hook found an isolated black node to flip");
    check(rb_validate(t) == 3, "validate detects invariant 3 (black-height mismatch)");
    rb_destroy(t);
}

static void test_validate_detects_order_violation(void) {
    rbtree_t *t = build_seven_key_fixture();
    check(rb_validate(t) == 0, "validate passes before corruption");
    check(rb_test_swap_keys(t, "a", "g") == 1, "corruption hook swapped two key pointers");
    check(rb_validate(t) == 4, "validate detects invariant 4 (in-order keys not increasing)");
    rb_destroy(t);
}

static void test_validate_detects_size_mismatch(void) {
    rbtree_t *t = rb_create(NULL);
    int a = 1, b = 2, c = 3;
    check(rb_insert(t, "b", &a) == 0, "insert b for size-mismatch fixture");
    check(rb_insert(t, "a", &b) == 0, "insert a for size-mismatch fixture");
    check(rb_insert(t, "c", &c) == 0, "insert c for size-mismatch fixture");
    check(rb_validate(t) == 0, "validate passes before corruption");
    rb_test_bump_size(t);
    check(rb_validate(t) == 5, "validate detects invariant 5 (size/count mismatch)");
    rb_destroy(t);
}
#endif

static void test_insert_single_root_is_black(void) {
    rbtree_t *t = rb_create(NULL);
    int value = 1;
    check(rb_insert(t, "only", &value) == 0, "insert single key for root-black test");
    check(rb_validate(t) == 0, "validate passes on single-node tree (root is black)");
    rb_destroy(t);
}

static void test_insert_ascending_triggers_rr_rotation(void) {
    /* a < b < c inserted in ascending order forces the RR straight-line case
     * at the root: c (red) under b (red) under a (root, black), uncle NIL.
     * A left rotation at a must promote b to root. If the rotation forgets to
     * update t->root, rb_find would search from the stale (demoted) root and
     * silently miss keys — the stale-root regression. */
    rbtree_t *t = rb_create(NULL);
    int va = 1, vb = 2, vc = 3;
    check(rb_insert(t, "a", &va) == 0, "insert a");
    check(rb_insert(t, "b", &vb) == 0, "insert b");
    check(rb_insert(t, "c", &vc) == 0, "insert c (triggers RR rotation at root)");
    check(rb_find(t, "a") == &va, "find a after RR rotation");
    check(rb_find(t, "b") == &vb, "find b after RR rotation");
    check(rb_find(t, "c") == &vc, "find c after RR rotation");
    check(rb_size(t) == 3, "size is 3 after RR-triggering inserts");
    check(rb_validate(t) == 0, "validate passes after RR rotation");
    struct order_ctx ctx = { .count = 0 };
    rb_foreach(t, collect_key, &ctx);
    check(ctx.count == 3 && strcmp(ctx.keys[0], "a") == 0 &&
          strcmp(ctx.keys[1], "b") == 0 && strcmp(ctx.keys[2], "c") == 0,
          "foreach still in order after RR rotation");
    rb_destroy(t);
}

static void test_insert_descending_triggers_ll_rotation(void) {
    /* Mirror of the RR case: c, b, a inserted descending forces the LL
     * straight-line case at the root, needing a right rotation at c. */
    rbtree_t *t = rb_create(NULL);
    int va = 1, vb = 2, vc = 3;
    check(rb_insert(t, "c", &vc) == 0, "insert c");
    check(rb_insert(t, "b", &vb) == 0, "insert b");
    check(rb_insert(t, "a", &va) == 0, "insert a (triggers LL rotation at root)");
    check(rb_find(t, "a") == &va, "find a after LL rotation");
    check(rb_find(t, "b") == &vb, "find b after LL rotation");
    check(rb_find(t, "c") == &vc, "find c after LL rotation");
    check(rb_size(t) == 3, "size is 3 after LL-triggering inserts");
    check(rb_validate(t) == 0, "validate passes after LL rotation");
    struct order_ctx ctx = { .count = 0 };
    rb_foreach(t, collect_key, &ctx);
    check(ctx.count == 3 && strcmp(ctx.keys[0], "a") == 0 &&
          strcmp(ctx.keys[1], "b") == 0 && strcmp(ctx.keys[2], "c") == 0,
          "foreach still in order after LL rotation");
    rb_destroy(t);
}

static void test_insert_triggers_red_uncle_recolor(void) {
    /* m, d, t: m is root (black), d and t are its red children (no
     * violation). Inserting b (< d) creates a red-red violation (b, d) whose
     * uncle t is red -> Case 1: recolor d,t black, m red, then re-blacken m
     * since it's root. No rotation needed for this case. */
    rbtree_t *t = rb_create(NULL);
    int vm = 1, vd = 2, vtt = 3, vb = 4;
    check(rb_insert(t, "m", &vm) == 0, "insert m");
    check(rb_insert(t, "d", &vd) == 0, "insert d");
    check(rb_insert(t, "t", &vtt) == 0, "insert t");
    check(rb_insert(t, "b", &vb) == 0, "insert b (triggers red-uncle recolor)");
    check(rb_find(t, "m") == &vm, "find m after red-uncle recolor");
    check(rb_find(t, "d") == &vd, "find d after red-uncle recolor");
    check(rb_find(t, "t") == &vtt, "find t after red-uncle recolor");
    check(rb_find(t, "b") == &vb, "find b after red-uncle recolor");
    check(rb_size(t) == 4, "size is 4 after red-uncle-triggering inserts");
    check(rb_validate(t) == 0, "validate passes after red-uncle recolor");
    rb_destroy(t);
}

static void test_insert_triggers_lr_triangle_double_rotation(void) {
    /* c, a, b: a is c's red left child; b (between a and c) becomes a's red
     * right child -- a "triangle" on the left side, uncle (c's right, NIL)
     * black. Needs rotate_left(a) to straighten, then rotate_right(c). */
    rbtree_t *t = rb_create(NULL);
    int vc = 1, va = 2, vb = 3;
    check(rb_insert(t, "c", &vc) == 0, "insert c");
    check(rb_insert(t, "a", &va) == 0, "insert a");
    check(rb_insert(t, "b", &vb) == 0, "insert b (triggers LR double rotation)");
    check(rb_find(t, "a") == &va, "find a after LR rotation");
    check(rb_find(t, "b") == &vb, "find b after LR rotation");
    check(rb_find(t, "c") == &vc, "find c after LR rotation");
    check(rb_size(t) == 3, "size is 3 after LR-triggering inserts");
    check(rb_validate(t) == 0, "validate passes after LR rotation");
    struct order_ctx ctx = { .count = 0 };
    rb_foreach(t, collect_key, &ctx);
    check(ctx.count == 3 && strcmp(ctx.keys[0], "a") == 0 &&
          strcmp(ctx.keys[1], "b") == 0 && strcmp(ctx.keys[2], "c") == 0,
          "foreach still in order after LR rotation");
    rb_destroy(t);
}

static void test_insert_triggers_rl_triangle_double_rotation(void) {
    /* Mirror of LR: a, c, b -- triangle on the right side, needs
     * rotate_right(c) to straighten, then rotate_left(a). */
    rbtree_t *t = rb_create(NULL);
    int va = 1, vc = 2, vb = 3;
    check(rb_insert(t, "a", &va) == 0, "insert a");
    check(rb_insert(t, "c", &vc) == 0, "insert c");
    check(rb_insert(t, "b", &vb) == 0, "insert b (triggers RL double rotation)");
    check(rb_find(t, "a") == &va, "find a after RL rotation");
    check(rb_find(t, "b") == &vb, "find b after RL rotation");
    check(rb_find(t, "c") == &vc, "find c after RL rotation");
    check(rb_size(t) == 3, "size is 3 after RL-triggering inserts");
    check(rb_validate(t) == 0, "validate passes after RL rotation");
    struct order_ctx ctx = { .count = 0 };
    rb_foreach(t, collect_key, &ctx);
    check(ctx.count == 3 && strcmp(ctx.keys[0], "a") == 0 &&
          strcmp(ctx.keys[1], "b") == 0 && strcmp(ctx.keys[2], "c") == 0,
          "foreach still in order after RL rotation");
    rb_destroy(t);
}

static void test_destroy_single_node(void) {
    rbtree_t *t = rb_create(NULL);
    int value = 1;
    check(rb_insert(t, "only", &value) == 0, "insert single node for destroy test");
    rb_destroy(t); /* clean under asan/memcheck is the test */
}

static void test_destroy_left_heavy_chain(void) {
    /* Descending insertion order with no fixup builds a pure left-child chain
     * (e -> d -> c -> b -> a). Exercises the Reach's rotation branch on every
     * iteration but the last, on purpose — not incidentally like the existing
     * descending-order validate test. */
    rbtree_t *t = rb_create(NULL);
    const char *keys[5] = {"e", "d", "c", "b", "a"};
    int values[5] = {0};
    for (int i = 0; i < 5; i++) {
        check(rb_insert(t, keys[i], &values[i]) == 0, "insert descending for left-heavy chain");
    }
    rb_destroy(t); /* clean under asan/memcheck is the test */
}

static void test_destroy_calls_value_free_once_per_node(void) {
    free_count = 0;
    rbtree_t *t = rb_create(counting_free);
    const char *keys[5] = {"cherry", "apple", "elderberry", "banana", "date"};
    for (int i = 0; i < 5; i++) {
        check(rb_insert(t, keys[i], make_int(i)) == 0, "insert heap value for destroy-count test");
    }
    rb_destroy(t);
    check(free_count == 5, "value_free called exactly once per node on destroy");
}

/* ---- rb_delete: table-driven cases ----
 * Each case builds a tree via a fixed insertion sequence (chosen so the
 * insertion fixup produces the exact structural situation named), deletes
 * one key, then asserts rb_delete's return code, rb_size, rb_validate, that
 * the deleted key is gone, and that every surviving key is still findable.
 * Mirrored cases are literal left/right mirrors of their sibling case
 * (mirrored keys), per NOTES.md's decision tree and the spec's explicit
 * call-out that the mirror is "the half people forget."
 */
struct delete_case {
    const char *name;
    const char *insert_keys[TEST_MAX_KEYS];
    int         insert_count;
    const char *delete_key;
};

static const struct delete_case delete_cases[] = {
    /* Red leaf: with keys b,a,c inserted, a and c end up red leaf children
     * of black root b. Deleting a red leaf needs no fixup at all. */
    { "delete red leaf", { "b", "a", "c" }, 3, "a" },
    { "delete red leaf (mirror)", { "b", "a", "c" }, 3, "c" },

    /* Black leaf with red sibling (verified against an instrumented scratch
     * build that dumps real node colors -- an earlier candidate sequence
     * looked right by hand-simulation but actually produced a BLACK sibling
     * with red nephews, i.e. Case 4, not this Case 5):
     * c,b,d,f,g,l,n inserted, then delete b: b ends up a black leaf whose
     * sibling f is genuinely RED (f's children d is a black leaf, l is
     * black with red children g,n). Deleting b forces Case 5 (red sibling
     * rotated up over the parent, colors swapped, then the loop re-examines
     * the new -- now black -- sibling). */
    { "delete black leaf with red sibling",
      { "c", "b", "d", "f", "g", "l", "n" }, 7, "b" },
    /* Mirror: p,n,l,k,c,i,e inserted, then delete p: p is a black leaf
     * whose sibling k is RED, but on the LEFT this time, forcing the
     * mirrored (right) rotation direction. */
    { "delete black leaf with red sibling (mirror)",
      { "p", "n", "l", "k", "c", "i", "e" }, 7, "p" },

    /* Node with two children: root has two children; delete the root itself
     * so the successor-splice path is exercised on the two-children case
     * generally (also doubles as the "root deletion" requirement). */
    { "delete node with two children (root)",
      { "d", "b", "f", "a", "c", "e", "g" }, 7, "d" },
    { "delete node with two children (non-root)",
      { "d", "b", "f", "a", "c", "e", "g" }, 7, "f" },

    /* Root deletion, single node: the only node in the tree is the root. */
    { "delete root (single node tree)", { "only" }, 1, "only" },

    /* delete_fixup Case 4, far nephew red directly (no near-red conversion
     * rotation needed): d,b,f,a,c,e,g,h inserted, then delete e. e is a black
     * leaf; parent f is red; sibling g is black with g->right=h red (far
     * nephew) and g->left=NULL (near nephew, black). is_black(sib->right) is
     * false, so the code skips the rotate_right(sib) conversion at
     * rbtree.c:203-208 and goes straight to the terminal recolor+rotate_left.
     * Verified against a real build (not hand-simulated only): rb_delete
     * returns 0, rb_validate passes, and the resulting shape/colors (f
     * black, g promoted red, h recolored black) match predicting the
     * terminal block by hand. */
    { "delete Case 4 (sibling black, far nephew red, direct)",
      { "d", "b", "f", "a", "c", "e", "g", "h" }, 8, "e" },
    /* Mirror: e,g,c,h,f,d,b,a is the exact structural mirror of the above
     * (built by mapping each key to its mirrored in-order role, then
     * verifying the pre-delete tree's shape/colors are the left-right
     * mirror before deleting) -- delete d hits the x==parent->right branch,
     * sib=b, far nephew=a red, same direct-terminal path via rotate_right. */
    { "delete Case 4 (sibling black, far nephew red, direct, mirror)",
      { "e", "g", "c", "h", "f", "d", "b", "a" }, 8, "d" },

    /* delete_fixup Case 4, near nephew red / far nephew black -- forces the
     * rotate_right(t, sib) conversion at rbtree.c:203-208 before the same
     * terminal block Case-4-direct already exercises. d,b,f,a,c,e,g,fz
     * inserted (key "fz" sorts between f and g under strcmp), then delete e.
     * e is a black leaf; parent f is red; sibling g is black with
     * g->left=fz red (near nephew) and g->right=NULL (far nephew, black).
     * is_black(sib->right) is true, so the conversion rotation fires (fz
     * recolored black, g recolored red, rotate_right(g), sib reassigned to
     * fz) before the terminal recolor+rotate_left. Verified against a real
     * build: rb_delete returns 0, rb_validate passes, and the resulting
     * shape (fz promoted red, f and g as its black children) matches
     * predicting the conversion-then-terminal sequence by hand. */
    { "delete Case 4 (sibling black, near nephew red, conversion)",
      { "d", "b", "f", "a", "c", "e", "g", "fz" }, 8, "e" },
    /* Mirror: e,g,c,h,f,d,a,b is the structural mirror (mapping
     * f<->c, fz<->b, g<->a, e<->d, d<->e, c<->f, b<->g, a<->h), verified
     * node-by-node against the left case's shape before deleting -- delete
     * d hits the x==parent->right branch, sib=a, near nephew=b red (far
     * nephew NULL), forcing the mirrored rotate_left(t, sib) conversion. */
    { "delete Case 4 (sibling black, near nephew red, conversion, mirror)",
      { "e", "g", "c", "h", "f", "d", "a", "b" }, 8, "d" },

    /* delete_fixup Case 3a: both nephews black, PARENT RED -- terminal (no
     * climb). Exhaustively brute-forced every insertion permutation of
     * n=4..8 sequential keys against the real rb_insert looking for a
     * non-root red node with two black children whose own children are
     * black; zero matches exist below n=8 (a red-uncle recolor producing
     * this shape needs a same-black-height sibling subtree under the root,
     * which needs real depth). a,b,c,d,e,f,g,h ascending, then delete a:
     * a is a black leaf; parent b is red; sibling c is black with both
     * children NIL (nephews). delete_fixup's x==parent->left branch skips
     * Case 5 (sib not red) and lands in the both-nephews-black check
     * (rbtree.c:198), which recolors c red and reassigns x=b, parent=d --
     * then the while guard's is_black(x) sees x=b is RED and exits the loop
     * immediately (no second iteration/climb), so the post-loop catch-all
     * at rbtree.c:243 paints b black. Two recolors, zero rotations, debt
     * absorbed in one step because b had "room" (was red) to become black.
     * Verified against a real build: rb_delete returns 0, rb_validate
     * passes, and b(B)/c(R) with f's subtree completely untouched matches
     * the hand-traced recolor exactly. */
    { "delete Case 3a (sibling black, both nephews black, parent red)",
      { "a", "b", "c", "d", "e", "f", "g", "h" }, 8, "a" },
    /* Mirror: h,g,f,e,d,c,b,a (the mirror of a fully-ascending sequence is
     * the fully-descending one) -- delete h hits the x==parent->right
     * branch, sib=f (both children NIL), parent g red; same terminal
     * recolor-absorb, verified as g's exact structural mirror before and
     * after delete. */
    { "delete Case 3a (sibling black, both nephews black, parent red, mirror)",
      { "h", "g", "f", "e", "d", "c", "b", "a" }, 8, "h" },

    /* delete_fixup Case 3b: both nephews black, PARENT BLACK -- the debt
     * does NOT terminate, it climbs (x=parent; parent=x->parent) and the
     * whole case-selection logic re-runs fresh one level up. NOTES.md's own
     * gap note: none of the original 9 table cases exercised a climb of
     * more than one ancestor. Random search + delta-debug shrinking (over
     * an instrumented scratch copy of delete_fixup, never src/rbtree.c
     * itself) found this is genuinely hard to produce small -- no sequence
     * under 38 nodes reproduces two real climbing passes; every insertion
     * permutation tried at n<38 either never reached delete_fixup's loop
     * body twice or resolved by the second level. Traced against the real
     * compiled rb_delete (case labels added via fprintf instrumentation,
     * no logic changed):
     *   iter1: x=NIL,   parent=00028(B) -> Case 3 (sib=00029, both nephews
     *          NIL) -> sib recolored RED, x=00028, parent=00026 (CLIMBS,
     *          since 00028 was black -- nothing to absorb the debt yet)
     *   iter2: x=00028, parent=00026(B) -> Case 3 mirror (sib=00023, both
     *          nephews black) -> sib recolored RED, x=00026, parent=00021
     *          (CLIMBS again, 00026 also black)
     *   iter3: x=00026, parent=00021(R) -> Case 4 mirror (sib=00012, red
     *          nephew) -> terminal rotate+recolor, returns.
     * rb_validate passes before and after, rb_delete returns 0. This is the
     * longest climb found across every search strategy tried (random trees
     * up to 500 nodes, plus a targeted search biased toward all-black
     * ancestor chains); a genuine climb-to-root was not found and is left
     * to the fuzzer's existing >=10^5-op coverage rather than forced into
     * a table row -- the spec's table-driven requirement (NOTES.md:58-60)
     * names red leaf / black-leaf-with-red-sibling / two-children / root
     * deletion as the minimum, not a climb-to-root case, and NOTES.md's own
     * "Confusions" note already resolves this specific gap by extending
     * the fuzzer, not by adding more table rows. */
    { "delete Case 3b (both nephews black, parent black -- multi-level climb)",
      { "00030","00033","00058","00038","00034","00017","00031","00021","00018","00029",
        "00028","00040","00027","00047","00026","00020","00006","00023","00012","00053",
        "00000","00001","00056","00009","00054","00022","00015","00013","00002","00010",
        "00008","00003","00024","00007","00005","00025","00016","00004" },
      38, "00027" },
    /* Mirror-starting climb (iter1 lands in the x==parent->right branch
     * instead): same shrink methodology, independently found and reduced
     * to 38 nodes.
     *   iter1: x=NIL,   parent=058(B) -> Case 3 mirror (sib=057) -> CLIMBS
     *   iter2: x=058,   parent=056(B) -> Case 3 mirror (sib=052) -> CLIMBS
     *   iter3: x=056,   parent=048(R) -> Case 4 mirror (sib=040) -> TERMINAL
     * rb_validate passes before/after, rb_delete returns 0. */
    { "delete Case 3b (both nephews black, parent black -- multi-level climb, mirror)",
      { "059","058","042","040","029","030","002","024","034","006",
        "012","023","020","031","036","056","007","048","004","001",
        "011","057","054","051","033","009","005","010","052","050",
        "037","041","043","046","035","047","044","039" },
      38, "059" },

    /* Black node with exactly one (red) child: b inserted then a inserted
     * descending leaves b black with a single red left child a (no fixup
     * needed for 2 nodes -- b stays black, a is red). Deleting b splices
     * a into its place. */
    { "delete black node with one red child (left)",
      { "b", "a" }, 2, "b" },
    { "delete black node with one red child (right, mirror)",
      { "a", "b" }, 2, "a" },
};

static void run_delete_case(const struct delete_case *c) {
    rbtree_t *t = rb_create(NULL);
    int values[TEST_MAX_KEYS] = {0};
    char msg[256];

    for (int i = 0; i < c->insert_count; i++) {
        snprintf(msg, sizeof msg, "[%s] insert %s", c->name, c->insert_keys[i]);
        check(rb_insert(t, c->insert_keys[i], &values[i]) == 0, msg);
    }

    snprintf(msg, sizeof msg, "[%s] rb_delete(%s) returns 0", c->name, c->delete_key);
    check(rb_delete(t, c->delete_key) == 0, msg);

    snprintf(msg, sizeof msg, "[%s] size decremented after delete", c->name);
    check(rb_size(t) == (size_t)(c->insert_count - 1), msg);

    snprintf(msg, sizeof msg, "[%s] rb_validate passes after delete", c->name);
    check(rb_validate(t) == 0, msg);

    snprintf(msg, sizeof msg, "[%s] deleted key %s no longer found", c->name, c->delete_key);
    check(rb_find(t, c->delete_key) == NULL, msg);

    for (int i = 0; i < c->insert_count; i++) {
        if (strcmp(c->insert_keys[i], c->delete_key) == 0) continue;
        snprintf(msg, sizeof msg, "[%s] surviving key %s still found", c->name, c->insert_keys[i]);
        check(rb_find(t, c->insert_keys[i]) != NULL, msg);
    }

    rb_destroy(t);
}

static void test_delete_table_driven(void) {
    size_t n = sizeof delete_cases / sizeof delete_cases[0];
    for (size_t i = 0; i < n; i++) run_delete_case(&delete_cases[i]);
}

static void test_delete_missing_key_returns_error(void) {
    rbtree_t *t = rb_create(NULL);
    int value = 1;
    check(rb_insert(t, "apple", &value) == 0, "insert apple for missing-key delete test");
    check(rb_delete(t, "banana") == -1, "rb_delete on absent key returns -1");
    check(rb_size(t) == 1, "size unchanged after failed delete");
    check(rb_validate(t) == 0, "validate still passes after failed delete");
    rb_destroy(t);
}

static void test_delete_frees_value(void) {
    free_count = 0;
    rbtree_t *t = rb_create(counting_free);
    check(rb_insert(t, "apple", make_int(1)) == 0, "insert apple with heap value");
    check(rb_delete(t, "apple") == 0, "delete apple");
    check(free_count == 1, "value_free called exactly once on delete");
    check(rb_size(t) == 0, "size is 0 after deleting only node");
    rb_destroy(t);
}

/* ---- Mutation 1: allocation-failure fault injection (tests/fault_alloc.{c,h}) ----
 *
 * src/rbtree.c routes all heap allocation through rb_malloc/rb_free (commit
 * a250531), so these are meaningful: arming the injector genuinely reaches
 * rb_create/rb_insert's allocations, and test_fault_sweep's overwrite/delete
 * steps assert both fault_alloc_total() and value_free call counts, so
 * "zero allocations" there is a checked claim, not a vacuous one.
 */

/* Checks the spec's five minimums for "the tree is unchanged" after an
 * injected allocation failure during rb_insert(t, failed_key, ...):
 * rb_validate, rb_size, rb_find on the failed key, rb_find on every other
 * previously-inserted key, and that the destructor was not invoked for the
 * rejected (still caller-owned) value. */
static void assert_unchanged(rbtree_t *t,
                              const char *model_keys[], int *model_values[], int model_count,
                              const char *failed_key, void *find_before, size_t size_before,
                              int free_count_before, const char *label) {
    char msg[256];

    snprintf(msg, sizeof msg, "[%s] rb_validate passes after injected failure", label);
    check(rb_validate(t) == 0, msg);

    snprintf(msg, sizeof msg, "[%s] rb_size unchanged after injected failure", label);
    check(rb_size(t) == size_before, msg);

    snprintf(msg, sizeof msg, "[%s] rb_find(failed key) unchanged after injected failure", label);
    check(rb_find(t, failed_key) == find_before, msg);

    for (int i = 0; i < model_count; i++) {
        snprintf(msg, sizeof msg, "[%s] surviving key %s still maps to its original value",
                 label, model_keys[i]);
        check(rb_find(t, model_keys[i]) == model_values[i], msg);
    }

    snprintf(msg, sizeof msg,
             "[%s] value_free was not called for the rejected caller-owned value", label);
    check(free_count == free_count_before, msg);
}

static void test_fault_rb_create_failure(void) {
    fault_alloc_arm(1);
    rbtree_t *t = rb_create(NULL);
    check(t == NULL, "[rb_create] returns NULL when its own allocation is injected to fail");
    check(fault_alloc_total() == 1, "[rb_create] fault_alloc_total is 1 at the injected failure");
    fault_alloc_disarm();
    if (t != NULL) rb_destroy(t); /* seam not yet wired -- rb_create unexpectedly succeeded */
}

static void test_fault_insert_node_alloc_failure(void) {
    free_count = 0;
    /* call 1 = rb_create's malloc; call 2 = rb_insert's node-struct malloc */
    fault_alloc_arm(2);
    rbtree_t *t = rb_create(counting_free);
    check(t != NULL, "[node-alloc] rb_create succeeds before the armed failure");

    size_t size_before = rb_size(t);
    void  *find_before = rb_find(t, "a");
    int free_count_before = free_count;
    int *value = make_int(99);

    int rc = rb_insert(t, "a", value);
    check(rc == -1, "[node-alloc] rb_insert returns -1 when the node-struct allocation is injected to fail");
    check(fault_alloc_total() == 2, "[node-alloc] fault_alloc_total is 2 at the injected failure");
    assert_unchanged(t, NULL, NULL, 0, "a", find_before, size_before, free_count_before, "node-alloc");

    if (rc != 0) free(value); /* tree never took ownership; harness reclaims it */
    fault_alloc_disarm();
    rb_destroy(t);
}

static void test_fault_insert_key_alloc_failure(void) {
    free_count = 0;
    /* call 1 = rb_create's malloc; call 2 = node-struct malloc; call 3 = key-copy malloc */
    fault_alloc_arm(3);
    rbtree_t *t = rb_create(counting_free);
    check(t != NULL, "[key-alloc] rb_create succeeds before the armed failure");

    size_t size_before = rb_size(t);
    void  *find_before = rb_find(t, "a");
    int free_count_before = free_count;
    int *value = make_int(99);

    int rc = rb_insert(t, "a", value);
    check(rc == -1, "[key-alloc] rb_insert returns -1 when the key-copy allocation is injected to fail");
    check(fault_alloc_total() == 3, "[key-alloc] fault_alloc_total is 3 at the injected failure");
    assert_unchanged(t, NULL, NULL, 0, "a", find_before, size_before, free_count_before, "key-alloc");

    if (rc != 0) free(value); /* tree never took ownership; harness reclaims it */
    fault_alloc_disarm();
    rb_destroy(t);
}

static void test_fault_overwrite_allocates_nothing(void) {
    free_count = 0;
    rbtree_t *t = rb_create(counting_free);
    check(rb_insert(t, "apple", make_int(1)) == 0, "[overwrite] seed insert succeeds");

    fault_alloc_arm(1);
    int *second = make_int(2);
    check(rb_insert(t, "apple", second) == 0,
          "[overwrite] overwrite still succeeds with the very next allocation armed to fail");
    check(fault_alloc_total() == 0,
          "[overwrite] rb_malloc was never called (fault_alloc_total stays 0)");
    fault_alloc_disarm();
    rb_destroy(t);
}

static void test_fault_delete_allocates_nothing(void) {
    free_count = 0;
    rbtree_t *t = rb_create(counting_free);
    check(rb_insert(t, "apple", make_int(1)) == 0, "[delete] seed insert succeeds");

    fault_alloc_arm(1);
    check(rb_delete(t, "apple") == 0,
          "[delete] rb_delete still succeeds with the very next allocation armed to fail");
    check(fault_alloc_total() == 0,
          "[delete] rb_malloc was never called (fault_alloc_total stays 0)");
    fault_alloc_disarm();
    rb_destroy(t);
}

/* Deterministic fixed scenario (section 7c of the plan, extended): ascending
 * "a","b","c" triggers the RR rotation case in insert_fixup, so the sweep
 * also demonstrates failures never reach fixup (fixup only runs after the
 * unconditional link-in, which is always strictly after both of rb_insert's
 * allocations). The scenario then overwrites "a" and deletes "b" -- neither
 * allocates (src/rbtree.c's overwrite branch and rb_delete call rb_malloc
 * zero times), so this proves that in-scenario, not just in the standalone
 * test_fault_overwrite_allocates_nothing / test_fault_delete_allocates_nothing
 * tests on a pristine tree -- and checks ownership/destructor accounting
 * (exact value_free call counts), not just allocator call counts, around
 * both ops. */
typedef enum { SWEEP_INSERT, SWEEP_OVERWRITE, SWEEP_DELETE } sweep_op_kind_t;

struct sweep_op {
    sweep_op_kind_t kind;
    const char     *key;
    int             value;
};

static void test_fault_sweep(void) {
    static const struct sweep_op scenario[] = {
        { SWEEP_INSERT,    "a", 0  },
        { SWEEP_INSERT,    "b", 1  },
        { SWEEP_INSERT,    "c", 2  },
        { SWEEP_OVERWRITE, "a", 10 },
        { SWEEP_DELETE,    "b", 0  },
    };
    const int scenario_count = (int)(sizeof scenario / sizeof scenario[0]);
    bool terminated = false;

    /* Unbounded on purpose: the scenario performs finitely many allocations
     * by construction, so fault_alloc_total() < n (checked below) is
     * guaranteed to fire and break this loop -- no arbitrary iteration cap
     * is needed or wanted. */
    for (long n = 1; ; n++) {
        free_count = 0;
        fault_alloc_arm(n);

        rbtree_t *t = rb_create(counting_free);
        if (t == NULL) {
            check(fault_alloc_total() == n, "[sweep] rb_create's own allocation was the n-th call");
            fault_alloc_disarm();
            continue;
        }

        const char *model_keys[TEST_MAX_KEYS];
        int        *model_values[TEST_MAX_KEYS];
        int model_count = 0;
        bool observed_failure = false;

        for (int i = 0; i < scenario_count; i++) {
            const struct sweep_op *op = &scenario[i];
            long total_before = fault_alloc_total();
            int free_count_before = free_count;

            if (op->kind == SWEEP_INSERT) {
                size_t size_before = rb_size(t);
                void  *find_before = rb_find(t, op->key);
                int *value = make_int(op->value);

                int rc = rb_insert(t, op->key, value);
                if (rc == -1) {
                    check(fault_alloc_total() == n, "[sweep] injected failure landed at the armed call n");
                    assert_unchanged(t, model_keys, model_values, model_count,
                                     op->key, find_before, size_before, free_count_before, "sweep");
                    free(value); /* tree never took ownership */
                    observed_failure = true;
                    break;
                }
                check(free_count == free_count_before,
                      "[sweep] insert of a new key frees nothing");
                model_keys[model_count] = op->key;
                model_values[model_count] = value;
                model_count++;

            } else if (op->kind == SWEEP_OVERWRITE) {
                int *value = make_int(op->value);
                int rc = rb_insert(t, op->key, value);
                check(rc == 0, "[sweep] overwrite never allocates, so it cannot be "
                               "the n-th call and always succeeds");
                check(fault_alloc_total() == total_before,
                      "[sweep] overwrite performed no allocation in-scenario");
                check(free_count == free_count_before + 1,
                      "[sweep] overwrite freed exactly the old value, once");
                for (int j = 0; j < model_count; j++) {
                    if (strcmp(model_keys[j], op->key) == 0) {
                        model_values[j] = value;
                        break;
                    }
                }

            } else { /* SWEEP_DELETE */
                int rc = rb_delete(t, op->key);
                check(rc == 0, "[sweep] delete never allocates, so it cannot be "
                               "the n-th call and always succeeds");
                check(fault_alloc_total() == total_before,
                      "[sweep] delete performed no allocation in-scenario");
                check(free_count == free_count_before + 1,
                      "[sweep] delete freed exactly the removed value, once");
                for (int j = 0; j < model_count; j++) {
                    if (strcmp(model_keys[j], op->key) == 0) {
                        for (int k = j; k + 1 < model_count; k++) {
                            model_keys[k] = model_keys[k + 1];
                            model_values[k] = model_values[k + 1];
                        }
                        model_count--;
                        break;
                    }
                }
            }
        }

        long total = fault_alloc_total();
        check(rb_validate(t) == 0, "[sweep] rb_validate passes at end of this run");
        rb_destroy(t);
        fault_alloc_disarm();

        if (total < n) {
            check(!observed_failure,
                  "[sweep] scenario completed without reaching allocation n -- "
                  "no rb_insert should have returned -1 in this run");
            terminated = true;
            break; /* legitimate termination: allocation n was never reached */
        }
        check(observed_failure,
              "[sweep] allocation n was reached -- the corresponding call must have "
              "returned its documented failure code");
    }

    check(terminated, "[sweep] sweep terminated via the legitimate completion condition "
                       "(fault_alloc_total() < n after a completed scenario run)");
}

#ifdef RBTREE_TEST_HOOKS
/* Direct check of the slab slot arithmetic (Mutation 2 geometry slice).
 *
 * Every expected number in the table below is worked out by hand, not
 * recomputed from src/pool.c's formula: a test that re-derives the geometry it
 * is checking lets a geometry bug cancel out of its own check. They also pin
 * the in-band header's cost -- if the header grows past one alignment unit,
 * slot0_offset moves and these rows fail loudly, which is the point.
 *
 * The literals are worked out for the ABI asserted here; on another one they
 * would be wrong rather than merely unchecked, so assert it instead of
 * assuming it. */
static_assert(alignof(max_align_t) == 16 && sizeof(void *) == 8,
              "geometry table literals are worked out for alignof(max_align_t) == 16 "
              "and sizeof(void *) == 8; recompute the table for another ABI");

#define GEOM_ALIGN ((size_t)16)
#define GEOM_SLAB  ((size_t)4096)

/* obj_size -> stride, slot0_offset, objs_per_slab, slack.
 * usable = 4096 - 16 (header, rounded up to the alignment) = 4080. */
static const struct geom_case {
    const char *name;
    size_t obj_size;
    bool   ok;
    size_t stride, slot0_offset, objs_per_slab, slack;
} geom_cases[] = {
    /* Rejected sizes: all four outputs must be zeroed. */
    { "obj_size 0",                   0,             false,    0,  0,   0,    0 },
    { "obj_size 4081 (one past fit)", 4081,          false,    0,  0,   0,    0 },
    { "obj_size 4096 (whole slab)",   4096,          false,    0,  0,   0,    0 },
    { "obj_size SIZE_MAX-8",          SIZE_MAX - 8,  false,    0,  0,   0,    0 },
    { "obj_size SIZE_MAX",            SIZE_MAX,      false,    0,  0,   0,    0 },

    /* Below a pointer: the slot still has to hold the free-list next. */
    { "obj_size 1 -> pointer floor",  1,             true,    16, 16, 255,    0 },
    { "obj_size 7 -> pointer floor",  7,             true,    16, 16, 255,    0 },
    { "obj_size 8 (exactly a ptr)",   8,             true,    16, 16, 255,    0 },

    /* Ordinary rounding, with and without slack. */
    { "obj_size 9",                   9,             true,    16, 16, 255,    0 },
    { "obj_size 16 (already stride)", 16,            true,    16, 16, 255,    0 },
    { "obj_size 17 (rounds to 32)",   17,            true,    32, 16, 127,   16 },
    { "obj_size 24",                  24,            true,    32, 16, 127,   16 },
    { "obj_size 40 (rounds to 48)",   40,            true,    48, 16,  85,    0 },
    { "obj_size 48 (the node size)",  48,            true,    48, 16,  85,    0 },
    { "obj_size 49 (rounds to 64)",   49,            true,    64, 16,  63,   48 },
    { "obj_size 100 (rounds to 112)", 100,           true,   112, 16,  36,   48 },

    /* One slot only, which is where an off-by-one in the usable span shows. */
    { "obj_size 2040",                2040,          true,  2048, 16,   1, 2032 },
    { "obj_size 4064 (slack 16)",     4064,          true,  4064, 16,   1,   16 },
    { "obj_size 4079 (exact fit)",    4079,          true,  4080, 16,   1,    0 },
    { "obj_size 4080 (exact fit)",    4080,          true,  4080, 16,   1,    0 },
};

static void run_geom_case(const struct geom_case *c) {
    char msg[256];
    /* Pre-set to a value the helper must overwrite either way, so a forgotten
     * store shows up as a mismatch rather than as an accidental zero. */
    size_t stride = 0xBAD, slot0 = 0xBAD, objs = 0xBAD, slack = 0xBAD;

    bool ok = rb_pool_geometry(c->obj_size, &stride, &slot0, &objs, &slack);

    snprintf(msg, sizeof msg, "[geometry: %s] returns %s", c->name, c->ok ? "true" : "false");
    check(ok == c->ok, msg);

    snprintf(msg, sizeof msg,
             "[geometry: %s] stride/slot0/objs/slack == %zu/%zu/%zu/%zu (got %zu/%zu/%zu/%zu)",
             c->name, c->stride, c->slot0_offset, c->objs_per_slab, c->slack,
             stride, slot0, objs, slack);
    check(stride == c->stride && slot0 == c->slot0_offset &&
          objs == c->objs_per_slab && slack == c->slack, msg);

    if (!c->ok) return;

    snprintf(msg, sizeof msg, "[geometry: %s] stride and slot0 are alignment multiples", c->name);
    check(stride % GEOM_ALIGN == 0 && slot0 % GEOM_ALIGN == 0, msg);

    snprintf(msg, sizeof msg, "[geometry: %s] slot0 leaves room for the in-band next pointer",
             c->name);
    check(slot0 >= sizeof(void *), msg);

    /* Minimal padding, computed without overflow: the stride must cover the
     * object (or the free-list pointer, whichever is larger) and must not
     * over-round it by a whole alignment unit. */
    size_t effective_min = (c->obj_size < sizeof(void *)) ? sizeof(void *) : c->obj_size;
    snprintf(msg, sizeof msg, "[geometry: %s] stride is minimal for effective_min %zu",
             c->name, effective_min);
    check(stride >= effective_min && stride - effective_min < GEOM_ALIGN, msg);

    snprintf(msg, sizeof msg, "[geometry: %s] slack is smaller than one slot", c->name);
    check(objs >= 1 && slack < stride, msg);

    /* The partition: every byte of the slab is header, slot, or slack. */
    snprintf(msg, sizeof msg,
             "[geometry: %s] slot0 + objs*stride + slack == 4096", c->name);
    check(slot0 + objs * stride + slack == GEOM_SLAB, msg);
}

static void test_pool_geometry_table(void) {
    size_t n = sizeof geom_cases / sizeof geom_cases[0];
    for (size_t i = 0; i < n; i++) run_geom_case(&geom_cases[i]);
}

/* ---- Mutation 2 pool lifecycle (slice A: create / stats / destroy) ----
 *
 * The pool owns no slabs yet and hands out nothing, so what is testable here
 * is the lifecycle: what pool_create accepts and rejects, what it costs in
 * rb_malloc calls, what an untouched pool reports, and that teardown leaks
 * nothing. The alloc/free scenarios arrive with pool_alloc.
 */

/* The pool tests are written against one object size, pinned by hand rather
 * than recomputed, so that a geometry change fails HERE rather than silently
 * re-deriving a different capacity that the pool tests would then agree with.
 * 48 is the current sizeof(struct rb_node), which is what rb_create_pooled
 * will ask for; 85 is worked out by hand as (4096 - 16) / 48. */
#define POOL_OBJ_SIZE      ((size_t)48)
#define POOL_OBJS_PER_SLAB ((size_t)85)

/* Fetches the capacity the invariant is stated over, asserting the pinned
 * value on the way through. Using rb_pool_geometry here is not circular: its
 * output is already nailed down independently by geom_cases[]'s hand-worked
 * literals, and the check below pins the one number these tests consume. */
static size_t pool_capacity(void) {
    size_t stride = 0, slot0 = 0, objs = 0, slack = 0;
    check(rb_pool_geometry(POOL_OBJ_SIZE, &stride, &slot0, &objs, &slack),
          "[pool] geometry accepts the 48-byte object size the pool tests use");
    check(objs == POOL_OBJS_PER_SLAB,
          "[pool] one slab holds 85 slots of 48 bytes (hand-pinned capacity)");
    return objs;
}

/* The M2 headline invariant. Called after every scenario, not once at the
 * end: the earliest assertion failure is the informative one. Trivially true
 * while every term is zero -- it becomes load-bearing in the carving slice,
 * and it is here now so that no scenario is ever added without it. */
static void check_pool_invariant(const rb_pool_t *p, size_t objs_per_slab, const char *label) {
    char msg[256];
    size_t slabs = 0, live = 0, free_objs = 0;
    pool_stats(p, &slabs, &live, &free_objs);
    snprintf(msg, sizeof msg,
             "[pool: %s] live + free_objs == slabs * objs_per_slab (%zu + %zu == %zu * %zu)",
             label, live, free_objs, slabs, objs_per_slab);
    check(live + free_objs == slabs * objs_per_slab, msg);
}

/* A fresh pool reports honest zeroes, which is what "lazy" means observably:
 * no slab has been bought, so there is no capacity to report yet. An eager
 * pool would report 1/0/85 here instead, so this is the test that pins the
 * decision rather than merely tolerating it. */
static void test_pool_create_empty_stats(void) {
    size_t cap = pool_capacity();
    rb_pool_t *p = pool_create(POOL_OBJ_SIZE);
    check(p != NULL, "[pool: empty] pool_create succeeds for a 48-byte object");
    if (p == NULL) return;

    size_t slabs = 0xBAD, live = 0xBAD, free_objs = 0xBAD;
    pool_stats(p, &slabs, &live, &free_objs);
    check(slabs == 0, "[pool: empty] a fresh pool owns no slabs (lazy first slab)");
    check(live == 0, "[pool: empty] a fresh pool has no live objects");
    check(free_objs == 0, "[pool: empty] a fresh pool reports no reusable slots");
    check_pool_invariant(p, cap, "empty");

    pool_destroy(p);
}

/* An obj_size no complete slot can hold is rejected, and -- because the
 * geometry test runs before the allocation -- rejected without spending an
 * rb_malloc call. fault_alloc_arm(1) is what makes that observable: were the
 * order reversed, the pool struct would be allocated and then thrown away,
 * and fault_alloc_total() would read 1 instead of 0. */
static void test_pool_create_rejects_unusable_obj_size(void) {
    static const struct { const char *name; size_t obj_size; } bad[] = {
        { "obj_size 0",         0            },
        { "obj_size 4081",      4081         },
        { "obj_size 4096",      4096         },
        { "obj_size SIZE_MAX",  SIZE_MAX     },
    };
    char msg[256];

    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        fault_alloc_arm(1);
        rb_pool_t *p = pool_create(bad[i].obj_size);

        snprintf(msg, sizeof msg, "[pool: reject] pool_create(%s) returns NULL", bad[i].name);
        check(p == NULL, msg);

        snprintf(msg, sizeof msg,
                 "[pool: reject] pool_create(%s) allocates nothing (geometry checked first)",
                 bad[i].name);
        check(fault_alloc_total() == 0, msg);

        fault_alloc_disarm();
        if (p != NULL) pool_destroy(p); /* unexpected success -- do not leak it */
    }
}

/* pool_create's single allocation, injected to fail. The total of exactly 1
 * is the other half of the lazy decision: an eager pool would have attempted
 * a second allocation for its first slab, so this count would be 2 on the
 * success path and this test would have an unwind to check. There is none. */
static void test_pool_create_allocation_failure(void) {
    fault_alloc_arm(1);
    rb_pool_t *p = pool_create(POOL_OBJ_SIZE);
    check(p == NULL, "[pool: create-fail] pool_create returns NULL when its allocation fails");
    check(fault_alloc_total() == 1,
          "[pool: create-fail] exactly one allocation was attempted before the failure");
    fault_alloc_disarm();
    if (p != NULL) pool_destroy(p); /* unexpected success -- do not leak it */
}

/* A whole create/destroy cycle costs exactly one rb_malloc, and leaks
 * nothing. The arm(1000) is only there to zero the since-arm counter -- no
 * scenario here comes close to 1000 allocations, so nothing is injected.
 * The leak half of this claim is adjudicated by make asan / make memcheck,
 * not by an assertion in this file. */
static void test_pool_create_destroy_is_one_allocation(void) {
    size_t cap = pool_capacity();
    fault_alloc_arm(1000);
    rb_pool_t *p = pool_create(POOL_OBJ_SIZE);
    check(p != NULL, "[pool: lifecycle] pool_create succeeds with the injector idle");
    if (p == NULL) { fault_alloc_disarm(); return; }

    check(fault_alloc_total() == 1,
          "[pool: lifecycle] a successful pool_create is exactly one rb_malloc (no slab bought)");
    check_pool_invariant(p, cap, "lifecycle");

    pool_destroy(p);
    check(fault_alloc_total() == 1,
          "[pool: lifecycle] pool_destroy on a slab-less pool allocates nothing");
    fault_alloc_disarm();
}

/* Smallest and largest object sizes geometry accepts both survive a full
 * lifecycle, and both report the same honest zeroes -- the stats of an
 * untouched pool do not depend on its geometry. */
static void test_pool_lifecycle_at_geometry_extremes(void) {
    static const size_t sizes[] = { 1, 8, 4080 };
    char msg[256];

    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        size_t stride = 0, slot0 = 0, objs = 0, slack = 0;
        check(rb_pool_geometry(sizes[i], &stride, &slot0, &objs, &slack),
              "[pool: extremes] geometry accepts the size under test");

        rb_pool_t *p = pool_create(sizes[i]);
        snprintf(msg, sizeof msg, "[pool: extremes] pool_create(%zu) succeeds", sizes[i]);
        check(p != NULL, msg);
        if (p == NULL) continue;

        size_t slabs = 0xBAD, live = 0xBAD, free_objs = 0xBAD;
        pool_stats(p, &slabs, &live, &free_objs);
        snprintf(msg, sizeof msg,
                 "[pool: extremes] pool_create(%zu) starts at 0 slabs / 0 live / 0 free",
                 sizes[i]);
        check(slabs == 0 && live == 0 && free_objs == 0, msg);
        check_pool_invariant(p, objs, "extremes");

        pool_destroy(p);
    }
}

/* ---- Mutation 2 slab growth and carving (slice B: pool_alloc) ----
 *
 * pool_free does not exist yet, so every slot handed out here stays live
 * until pool_destroy. That makes the scenarios below purely monotone: live
 * only rises, free_objs only falls (except where a new slab resets it), and
 * nothing can come back to be reused.
 */

/* One slab's worth of slots, twice over -- enough to cross a slab boundary
 * and fill the second one. A constant expression, not a VLA. */
#define POOL_TWO_SLABS (2 * POOL_OBJS_PER_SLAB)

/* Reads back the geometry the carve arithmetic is checked against. Pinned by
 * pool_capacity()'s hand-worked literal, as everywhere else in these tests. */
static size_t pool_stride(void) {
    size_t stride = 0, slot0 = 0, objs = 0, slack = 0;
    check(rb_pool_geometry(POOL_OBJ_SIZE, &stride, &slot0, &objs, &slack),
          "[pool] geometry accepts the object size the carving tests use");
    return stride;
}

/* One aggregated assertion rather than n^2 of them: pointer-distinctness
 * over a whole run of allocations. Equality comparison between unrelated
 * pointers is well defined, so this is safe across slab boundaries in a way
 * that pointer subtraction would not be. */
static void check_all_distinct(void *const *slots, size_t n, const char *label) {
    char msg[256];
    size_t collisions = 0;
    for (size_t i = 0; i < n; i++)
        for (size_t j = i + 1; j < n; j++)
            if (slots[i] == slots[j]) collisions++;
    snprintf(msg, sizeof msg, "[pool: %s] all %zu slots are distinct addresses (%zu collisions)",
             label, n, collisions);
    check(collisions == 0, msg);
}

static void check_all_aligned(void *const *slots, size_t n, const char *label) {
    char msg[256];
    size_t misaligned = 0;
    for (size_t i = 0; i < n; i++)
        if ((uintptr_t)slots[i] % alignof(max_align_t) != 0) misaligned++;
    snprintf(msg, sizeof msg, "[pool: %s] all %zu slots are max_align_t-aligned (%zu are not)",
             label, n, misaligned);
    check(misaligned == 0, msg);
}

static void test_pool_alloc_first_slot(void) {
    size_t cap = pool_capacity();
    rb_pool_t *p = pool_create(POOL_OBJ_SIZE);
    check(p != NULL, "[pool: first] pool_create succeeds");
    if (p == NULL) return;

    void *slot = pool_alloc(p);
    check(slot != NULL, "[pool: first] the first pool_alloc returns a slot");
    check((uintptr_t)slot % alignof(max_align_t) == 0,
          "[pool: first] the first slot is max_align_t-aligned");

    size_t slabs = 0xBAD, live = 0xBAD, free_objs = 0xBAD;
    pool_stats(p, &slabs, &live, &free_objs);
    check(slabs == 1, "[pool: first] the first allocation buys exactly one slab");
    check(live == 1, "[pool: first] one slot is live");
    check(free_objs == cap - 1, "[pool: first] the rest of the slab is reported as free");
    check_pool_invariant(p, cap, "first");

    pool_destroy(p);
}

/* The core carving test. Within one slab the slot addresses are observable
 * and must be exactly stride apart and ascending -- that is what pins the
 * arithmetic, far more tightly than distinctness alone. Then the slot after
 * the last one must come from a NEW slab rather than from the tail slack,
 * which is the off-by-one the spec's own M2 review prompt names. */
static void test_pool_alloc_fills_one_slab_then_grows(void) {
    size_t cap = pool_capacity();
    size_t stride = pool_stride();
    char msg[256];

    rb_pool_t *p = pool_create(POOL_OBJ_SIZE);
    check(p != NULL, "[pool: fill] pool_create succeeds");
    if (p == NULL) return;

    void *slots[POOL_TWO_SLABS];
    size_t nulls = 0;
    for (size_t i = 0; i < cap; i++) {
        slots[i] = pool_alloc(p);
        if (slots[i] == NULL) nulls++;
        check_pool_invariant(p, cap, "fill (per allocation)");
    }
    snprintf(msg, sizeof msg, "[pool: fill] all %zu allocations in the first slab succeed", cap);
    check(nulls == 0, msg);
    if (nulls != 0) { pool_destroy(p); return; }

    /* Same slab, so pointer subtraction between these is well defined. */
    size_t bad_steps = 0;
    for (size_t i = 1; i < cap; i++)
        if ((unsigned char *)slots[i] - (unsigned char *)slots[i - 1] != (ptrdiff_t)stride)
            bad_steps++;
    snprintf(msg, sizeof msg,
             "[pool: fill] consecutive slots in a slab are exactly stride (%zu) apart and "
             "ascending (%zu bad steps)", stride, bad_steps);
    check(bad_steps == 0, msg);

    check_all_aligned(slots, cap, "fill");

    size_t slabs = 0xBAD, live = 0xBAD, free_objs = 0xBAD;
    pool_stats(p, &slabs, &live, &free_objs);
    check(slabs == 1, "[pool: fill] a full slab is still exactly one slab");
    check(live == cap, "[pool: fill] every slot in the slab is live");
    check(free_objs == 0, "[pool: fill] a full slab reports no reusable slots");
    check_pool_invariant(p, cap, "fill");

    /* The 86th: must buy a slab, NOT hand out the tail slack. */
    slots[cap] = pool_alloc(p);
    check(slots[cap] != NULL, "[pool: cross] the allocation past a full slab succeeds");
    pool_stats(p, &slabs, &live, &free_objs);
    check(slabs == 2, "[pool: cross] crossing a full slab buys a second slab");
    check(live == cap + 1, "[pool: cross] the new slot is live");
    check(free_objs == cap - 1, "[pool: cross] the new slab's remaining slots are free");
    check_pool_invariant(p, cap, "cross");
    check_all_distinct(slots, cap + 1, "cross");

    /* Fill the second slab too: it must yield exactly cap slots as well. */
    nulls = 0;
    for (size_t i = cap + 1; i < POOL_TWO_SLABS; i++) {
        slots[i] = pool_alloc(p);
        if (slots[i] == NULL) nulls++;
    }
    check(nulls == 0, "[pool: two slabs] the second slab fills without failure");
    pool_stats(p, &slabs, &live, &free_objs);
    check(slabs == 2, "[pool: two slabs] two full slabs, no third bought");
    check(live == POOL_TWO_SLABS, "[pool: two slabs] every slot in both slabs is live");
    check(free_objs == 0, "[pool: two slabs] two full slabs report no reusable slots");
    check_pool_invariant(p, cap, "two slabs");
    check_all_distinct(slots, POOL_TWO_SLABS, "two slabs");
    check_all_aligned(slots, POOL_TWO_SLABS, "two slabs");

    pool_destroy(p);
}

/* The point of a slab allocator: object count does not drive allocation
 * count. Measured as a delta across the run rather than against an absolute
 * total, so the claim does not depend on what any earlier test allocated. */
static void test_pool_alloc_one_malloc_per_slab(void) {
    size_t cap = pool_capacity();

    fault_alloc_arm(100000); /* resets the counter; nothing here comes near 100000 */
    rb_pool_t *p = pool_create(POOL_OBJ_SIZE);
    check(p != NULL, "[pool: malloc count] pool_create succeeds with the injector idle");
    if (p == NULL) { fault_alloc_disarm(); return; }

    long before = fault_alloc_total();
    size_t nulls = 0;
    for (size_t i = 0; i < cap; i++)
        if (pool_alloc(p) == NULL) nulls++;
    long after_first_slab = fault_alloc_total();

    check(nulls == 0, "[pool: malloc count] filling the first slab succeeds");
    check(after_first_slab - before == 1,
          "[pool: malloc count] filling a whole slab costs exactly one rb_malloc");

    check(pool_alloc(p) != NULL, "[pool: malloc count] the allocation past the slab succeeds");
    check(fault_alloc_total() - after_first_slab == 1,
          "[pool: malloc count] crossing into a second slab costs exactly one more rb_malloc");

    fault_alloc_disarm();
    pool_destroy(p);
}

/* Distinct pointers are not enough: if the stride were smaller than the
 * object, adjacent slots would overlap while still having distinct
 * addresses. Writing a per-slot pattern across every byte of every slot and
 * reading them all back afterwards is what actually detects that. */
static void test_pool_alloc_slots_do_not_overlap(void) {
    size_t cap = pool_capacity();
    rb_pool_t *p = pool_create(POOL_OBJ_SIZE);
    check(p != NULL, "[pool: overlap] pool_create succeeds");
    if (p == NULL) return;

    unsigned char *slots[POOL_TWO_SLABS];
    size_t nulls = 0;
    for (size_t i = 0; i < POOL_TWO_SLABS; i++) {
        slots[i] = pool_alloc(p);
        if (slots[i] == NULL) { nulls++; continue; }
        memset(slots[i], (unsigned char)(i + 1), POOL_OBJ_SIZE);
    }
    check(nulls == 0, "[pool: overlap] two slabs' worth of allocations succeed");

    size_t corrupted = 0;
    for (size_t i = 0; i < POOL_TWO_SLABS; i++) {
        if (slots[i] == NULL) continue;
        for (size_t b = 0; b < POOL_OBJ_SIZE; b++)
            if (slots[i][b] != (unsigned char)(i + 1)) { corrupted++; break; }
    }
    check(corrupted == 0,
          "[pool: overlap] every slot's full object_size bytes survive writes to every other slot");
    check_pool_invariant(p, cap, "overlap");

    pool_destroy(p);
}

/* The degenerate geometry: one slot per slab, so every single allocation
 * must buy a slab and free_objs is never anything but zero. */
static void test_pool_alloc_single_slot_geometry(void) {
    size_t stride = 0, slot0 = 0, objs = 0, slack = 0;
    check(rb_pool_geometry(4080, &stride, &slot0, &objs, &slack),
          "[pool: one-per-slab] geometry accepts a 4080-byte object");
    check(objs == 1, "[pool: one-per-slab] a 4080-byte object leaves exactly one slot per slab");

    rb_pool_t *p = pool_create(4080);
    check(p != NULL, "[pool: one-per-slab] pool_create succeeds");
    if (p == NULL) return;

    char msg[256];
    for (size_t i = 1; i <= 3; i++) {
        check(pool_alloc(p) != NULL, "[pool: one-per-slab] allocation succeeds");
        size_t slabs = 0xBAD, live = 0xBAD, free_objs = 0xBAD;
        pool_stats(p, &slabs, &live, &free_objs);
        snprintf(msg, sizeof msg,
                 "[pool: one-per-slab] allocation %zu leaves %zu slabs / %zu live / 0 free",
                 i, i, i);
        check(slabs == i && live == i && free_objs == 0, msg);
        check_pool_invariant(p, objs, "one-per-slab");
    }

    pool_destroy(p);
}

/* Growth is the only fallible step in pool_alloc, and this is the stronger
 * test site that choosing a lazy first slab bought: the FIRST slab's
 * allocation can be injected to fail on a pool that already exists, so the
 * assertion is not just "NULL" but "unchanged, and still usable afterwards".
 * An eager pool could only have failed this at birth. */
static void test_pool_alloc_grow_failure_first_slab(void) {
    size_t cap = pool_capacity();
    rb_pool_t *p = pool_create(POOL_OBJ_SIZE);
    check(p != NULL, "[pool: grow-fail first] pool_create succeeds before arming");
    if (p == NULL) return;

    fault_alloc_arm(1);
    void *slot = pool_alloc(p);
    check(slot == NULL, "[pool: grow-fail first] pool_alloc returns NULL when the slab fails");
    check(fault_alloc_total() == 1,
          "[pool: grow-fail first] exactly one allocation was attempted");
    fault_alloc_disarm();

    size_t slabs = 0xBAD, live = 0xBAD, free_objs = 0xBAD;
    pool_stats(p, &slabs, &live, &free_objs);
    check(slabs == 0 && live == 0 && free_objs == 0,
          "[pool: grow-fail first] the pool is untouched by the failed allocation");
    check_pool_invariant(p, cap, "grow-fail first");

    slot = pool_alloc(p);
    check(slot != NULL, "[pool: grow-fail first] the pool still works once disarmed");
    pool_stats(p, &slabs, &live, &free_objs);
    check(slabs == 1 && live == 1 && free_objs == cap - 1,
          "[pool: grow-fail first] the retry produces normal stats");
    check_pool_invariant(p, cap, "grow-fail first retry");

    pool_destroy(p);
}

/* The same failure one slab in, where there is real state to preserve: a
 * full slab, a live count of cap, and an exhausted carve cursor. */
static void test_pool_alloc_grow_failure_later_slab(void) {
    size_t cap = pool_capacity();
    rb_pool_t *p = pool_create(POOL_OBJ_SIZE);
    check(p != NULL, "[pool: grow-fail later] pool_create succeeds");
    if (p == NULL) return;

    size_t nulls = 0;
    for (size_t i = 0; i < cap; i++)
        if (pool_alloc(p) == NULL) nulls++;
    check(nulls == 0, "[pool: grow-fail later] the first slab fills before arming");

    fault_alloc_arm(1);
    void *slot = pool_alloc(p);
    check(slot == NULL, "[pool: grow-fail later] pool_alloc returns NULL when the slab fails");
    fault_alloc_disarm();

    size_t slabs = 0xBAD, live = 0xBAD, free_objs = 0xBAD;
    pool_stats(p, &slabs, &live, &free_objs);
    check(slabs == 1 && live == cap && free_objs == 0,
          "[pool: grow-fail later] the full slab and its counters are untouched");
    check_pool_invariant(p, cap, "grow-fail later");

    slot = pool_alloc(p);
    check(slot != NULL, "[pool: grow-fail later] the pool still grows once disarmed");
    pool_stats(p, &slabs, &live, &free_objs);
    check(slabs == 2 && live == cap + 1 && free_objs == cap - 1,
          "[pool: grow-fail later] the retry buys the second slab");
    check_pool_invariant(p, cap, "grow-fail later retry");

    pool_destroy(p);
}

/* pool_destroy with every slot still live, which is the case the design is
 * built for: the pool owns slabs, not objects, so live slots are released
 * wholesale with the slab that contains them. This is also the first
 * scenario in which pool_destroy's chain walk actually runs -- the
 * save-next-before-rb_free rule is unexercised until a pool owns slabs.
 * The leak claim itself is adjudicated by make asan / make memcheck. */
static void test_pool_destroy_with_live_objects(void) {
    size_t cap = pool_capacity();

    /* Two full slabs, nothing freed. */
    rb_pool_t *p = pool_create(POOL_OBJ_SIZE);
    check(p != NULL, "[pool: destroy-live] pool_create succeeds");
    if (p == NULL) return;
    size_t nulls = 0;
    for (size_t i = 0; i < POOL_TWO_SLABS; i++)
        if (pool_alloc(p) == NULL) nulls++;
    check(nulls == 0, "[pool: destroy-live] two full slabs of live slots");
    check_pool_invariant(p, cap, "destroy-live");
    pool_destroy(p);
    check(1, "[pool: destroy-live] pool_destroy over two full slabs did not crash");

    /* A partially carved head slab, so teardown covers the uncarved case too. */
    p = pool_create(POOL_OBJ_SIZE);
    check(p != NULL, "[pool: destroy-partial] pool_create succeeds");
    if (p == NULL) return;
    nulls = 0;
    for (size_t i = 0; i < cap + 1; i++)
        if (pool_alloc(p) == NULL) nulls++;
    check(nulls == 0, "[pool: destroy-partial] one full slab plus one slot of the next");
    check_pool_invariant(p, cap, "destroy-partial");
    pool_destroy(p);
    check(1, "[pool: destroy-partial] pool_destroy over a partly carved slab did not crash");
}

/* Found by mutation-testing slice B: every object size used above (48 and
 * 4080) happens to have stride == obj_size, so a carve cursor advanced by
 * obj_size instead of stride passed the whole suite. These two sizes have
 * real internal padding, and the second also leaves tail slack, so between
 * them they pin both "advance by the stride" and "the cursor's final
 * position is inside the slack rather than one past the slab".
 *
 * Expected numbers are the hand-worked rows from geom_cases[], restated here
 * rather than recomputed. */
static void test_pool_alloc_stride_is_not_obj_size(void) {
    static const struct {
        size_t obj_size, stride, objs, slack;
    } padded[] = {
        { 40, 48,  85,  0 },  /* padding, no slack */
        { 17, 32, 127, 16 },  /* padding and slack */
    };
    char msg[256];

    for (size_t c = 0; c < sizeof padded / sizeof padded[0]; c++) {
        size_t stride = 0, slot0 = 0, objs = 0, slack = 0;
        check(rb_pool_geometry(padded[c].obj_size, &stride, &slot0, &objs, &slack),
              "[pool: padded] geometry accepts the padded object size");
        snprintf(msg, sizeof msg,
                 "[pool: padded] obj_size %zu has stride %zu / %zu slots / %zu slack",
                 padded[c].obj_size, padded[c].stride, padded[c].objs, padded[c].slack);
        check(stride == padded[c].stride && objs == padded[c].objs && slack == padded[c].slack,
              msg);

        /* The premise: if these were equal the test below would prove nothing. */
        snprintf(msg, sizeof msg, "[pool: padded] stride %zu differs from obj_size %zu",
                 stride, padded[c].obj_size);
        check(stride != padded[c].obj_size, msg);

        rb_pool_t *p = pool_create(padded[c].obj_size);
        snprintf(msg, sizeof msg, "[pool: padded] pool_create(%zu) succeeds", padded[c].obj_size);
        check(p != NULL, msg);
        if (p == NULL) continue;

        void *slots[POOL_TWO_SLABS];
        size_t nulls = 0;
        for (size_t i = 0; i < objs; i++) {
            slots[i] = pool_alloc(p);
            if (slots[i] == NULL) nulls++;
        }
        snprintf(msg, sizeof msg, "[pool: padded] all %zu slots of the slab are handed out", objs);
        check(nulls == 0, msg);

        if (nulls == 0) {
            size_t bad_steps = 0;
            for (size_t i = 1; i < objs; i++)
                if ((unsigned char *)slots[i] - (unsigned char *)slots[i - 1] != (ptrdiff_t)stride)
                    bad_steps++;
            snprintf(msg, sizeof msg,
                     "[pool: padded] slots advance by stride %zu, not obj_size %zu (%zu bad steps)",
                     stride, padded[c].obj_size, bad_steps);
            check(bad_steps == 0, msg);
            check_all_aligned(slots, objs, "padded");
            check_all_distinct(slots, objs, "padded");
        }

        size_t slabs = 0xBAD, live = 0xBAD, free_objs = 0xBAD;
        pool_stats(p, &slabs, &live, &free_objs);
        snprintf(msg, sizeof msg,
                 "[pool: padded] a full padded slab is 1 slab / %zu live / 0 free", objs);
        check(slabs == 1 && live == objs && free_objs == 0, msg);
        check_pool_invariant(p, objs, "padded");

        /* The slack must not become an extra slot, and with slack > 0 this is
         * also where the carve cursor sat inside the slack rather than at the
         * slab's one-past-the-end address. */
        check(pool_alloc(p) != NULL, "[pool: padded] the allocation past the slab succeeds");
        pool_stats(p, &slabs, &live, &free_objs);
        snprintf(msg, sizeof msg,
                 "[pool: padded] the slack is not handed out: 2 slabs / %zu live / %zu free",
                 objs + 1, objs - 1);
        check(slabs == 2 && live == objs + 1 && free_objs == objs - 1, msg);
        check_pool_invariant(p, objs, "padded cross");

        pool_destroy(p);
    }
}

/* ---- Mutation 2 free list and poisoning (slice C: pool_free) ----
 *
 * With pool_free in place the invariant finally has non-zero terms on both
 * sides, so live + free_objs == slabs * objs_per_slab stops being satisfiable
 * by a pool where everything is zero.
 */

static bool in_set(const void *needle, void *const *set, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (set[i] == needle) return true;
    return false;
}

static void test_pool_free_one_slot(void) {
    size_t cap = pool_capacity();
    rb_pool_t *p = pool_create(POOL_OBJ_SIZE);
    check(p != NULL, "[pool: free one] pool_create succeeds");
    if (p == NULL) return;

    void *slot = pool_alloc(p);
    check(slot != NULL, "[pool: free one] allocation succeeds");
    pool_free(p, slot);

    size_t slabs = 0xBAD, live = 0xBAD, free_objs = 0xBAD;
    pool_stats(p, &slabs, &live, &free_objs);
    check(slabs == 1, "[pool: free one] freeing a slot does not release its slab");
    check(live == 0, "[pool: free one] nothing is live after the free");
    check(free_objs == cap,
          "[pool: free one] the freed slot plus the uncarved rest are all reusable");
    check_pool_invariant(p, cap, "free one");

    pool_destroy(p);
}

/* The ordering test. At this point the slab has 84 uncarved slots AND one
 * slot on the free list, so returning the freed slot is a choice, not the
 * only option -- which is what makes this an assertion about priority
 * rather than about availability. */
static void test_pool_free_reuse_before_carving(void) {
    size_t cap = pool_capacity();
    rb_pool_t *p = pool_create(POOL_OBJ_SIZE);
    check(p != NULL, "[pool: reuse first] pool_create succeeds");
    if (p == NULL) return;

    void *a = pool_alloc(p);
    check(a != NULL, "[pool: reuse first] the first allocation succeeds");
    pool_free(p, a);

    size_t slabs = 0xBAD, live = 0xBAD, free_objs = 0xBAD;
    pool_stats(p, &slabs, &live, &free_objs);
    check(free_objs == cap, "[pool: reuse first] both a dead slot and uncarved slots are available");

    void *b = pool_alloc(p);
    check(b == a, "[pool: reuse first] the dead slot is reused before a fresh one is carved");
    pool_stats(p, &slabs, &live, &free_objs);
    check(slabs == 1 && live == 1 && free_objs == cap - 1,
          "[pool: reuse first] reuse leaves one live slot and no extra slab");
    check_pool_invariant(p, cap, "reuse first");

    pool_destroy(p);
}

static void test_pool_free_is_lifo(void) {
    size_t cap = pool_capacity();
    rb_pool_t *p = pool_create(POOL_OBJ_SIZE);
    check(p != NULL, "[pool: lifo] pool_create succeeds");
    if (p == NULL) return;

    void *a = pool_alloc(p), *b = pool_alloc(p), *c = pool_alloc(p);
    check(a != NULL && b != NULL && c != NULL, "[pool: lifo] three allocations succeed");
    pool_free(p, a);
    pool_free(p, b);
    pool_free(p, c);

    check(pool_alloc(p) == c, "[pool: lifo] the most recently freed slot comes back first");
    check(pool_alloc(p) == b, "[pool: lifo] then the one freed before it");
    check(pool_alloc(p) == a, "[pool: lifo] then the first one freed");
    check_pool_invariant(p, cap, "lifo");

    pool_destroy(p);
}

/* Reuse must cost nothing: no slab, and no rb_malloc at all. Measured as a
 * delta so the claim does not depend on what earlier tests allocated. */
static void test_pool_free_reuse_costs_no_allocation(void) {
    size_t cap = pool_capacity();
    const size_t k = 10;

    fault_alloc_arm(100000); /* resets the counter; nothing here approaches it */
    rb_pool_t *p = pool_create(POOL_OBJ_SIZE);
    check(p != NULL, "[pool: free reuse cost] pool_create succeeds");
    if (p == NULL) { fault_alloc_disarm(); return; }

    void *slots[POOL_TWO_SLABS];
    size_t nulls = 0;
    for (size_t i = 0; i < cap; i++)
        if ((slots[i] = pool_alloc(p)) == NULL) nulls++;
    check(nulls == 0, "[pool: free reuse cost] the first slab fills");

    for (size_t i = 0; i < k; i++) pool_free(p, slots[i]);
    check_pool_invariant(p, cap, "free reuse cost (after frees)");

    long before = fault_alloc_total();
    size_t off_list = 0;
    for (size_t i = 0; i < k; i++) {
        void *got = pool_alloc(p);
        if (got == NULL || !in_set(got, slots, k)) off_list++;
    }
    long after = fault_alloc_total();

    check(off_list == 0, "[pool: free reuse cost] every reallocated slot came from the freed set");
    check(after - before == 0, "[pool: free reuse cost] reusing 10 slots costs zero rb_malloc calls");

    size_t slabs = 0xBAD, live = 0xBAD, free_objs = 0xBAD;
    pool_stats(p, &slabs, &live, &free_objs);
    check(slabs == 1 && live == cap && free_objs == 0,
          "[pool: free reuse cost] the pool is back to one full slab");
    check_pool_invariant(p, cap, "free reuse cost");

    fault_alloc_disarm();
    pool_destroy(p);
}

/* Once the free list is exhausted, allocation falls through to carving --
 * and still does not allocate, because the slab has uncarved capacity. */
static void test_pool_free_spills_to_carving(void) {
    size_t cap = pool_capacity();
    rb_pool_t *p = pool_create(POOL_OBJ_SIZE);
    check(p != NULL, "[pool: spill] pool_create succeeds");
    if (p == NULL) return;

    void *first[5];
    size_t nulls = 0;
    for (size_t i = 0; i < 5; i++)
        if ((first[i] = pool_alloc(p)) == NULL) nulls++;
    check(nulls == 0, "[pool: spill] five allocations succeed");

    pool_free(p, first[0]);
    pool_free(p, first[1]);
    pool_free(p, first[2]);

    fault_alloc_arm(100000);
    long before = fault_alloc_total();
    check(pool_alloc(p) == first[2], "[pool: spill] free list drains LIFO: third free first");
    check(pool_alloc(p) == first[1], "[pool: spill] then the second");
    check(pool_alloc(p) == first[0], "[pool: spill] then the first");

    void *fresh = pool_alloc(p);
    check(fresh != NULL, "[pool: spill] the allocation past an empty free list succeeds");
    check(!in_set(fresh, first, 5), "[pool: spill] it is a freshly carved slot, not a reused one");
    check(fault_alloc_total() - before == 0,
          "[pool: spill] draining the list and carving again cost zero rb_malloc calls");
    fault_alloc_disarm();

    check_pool_invariant(p, cap, "spill");
    pool_destroy(p);
}

/* free_objs is free_count + carve_remaining, and until this slice only one
 * of those terms could ever be non-zero -- so dropping either changed no
 * observable. With one slot live, one dead, and the rest of the slab
 * uncarved, both terms are non-zero and the literal below pins their sum. */
static void test_pool_stats_free_objs_counts_both_terms(void) {
    size_t cap = pool_capacity();
    rb_pool_t *p = pool_create(POOL_OBJ_SIZE);
    check(p != NULL, "[pool: both terms] pool_create succeeds");
    if (p == NULL) return;

    void *a = pool_alloc(p);
    void *b = pool_alloc(p);
    check(a != NULL && b != NULL, "[pool: both terms] two allocations succeed");
    pool_free(p, a);

    size_t slabs = 0xBAD, live = 0xBAD, free_objs = 0xBAD;
    pool_stats(p, &slabs, &live, &free_objs);
    check(slabs == 1, "[pool: both terms] one slab");
    check(live == 1, "[pool: both terms] one slot live");
    /* 1 slot on the free list + (cap - 2) never carved. */
    check(free_objs == 1 + (cap - 2),
          "[pool: both terms] free_objs counts the dead slot AND the uncarved remainder");
    check_pool_invariant(p, cap, "both terms");

    pool_destroy(p);
}

#ifndef NDEBUG
/* Poisoning is gated on NDEBUG, so this test is too -- under -DNDEBUG there
 * is nothing to observe and asserting 0xDD would be wrong rather than
 * merely unchecked.
 *
 * This test deliberately reads a slot it has already freed. That is legal:
 * the slot lives inside a slab that is still one live rb_malloc block, which
 * is exactly why neither ASan nor valgrind can see a stale access here, and
 * why the 0xDD fill is the only thing that makes such a read obvious. Two
 * object sizes, so the second pins that the whole stride is poisoned rather
 * than only the object's own bytes. */
static void test_pool_free_poisons_dead_slot(void) {
    static const struct { size_t obj_size, stride; } cases[] = {
        { POOL_OBJ_SIZE, 48 },  /* stride == obj_size */
        { 40,            48 },  /* stride > obj_size: 8 bytes of padding too */
    };
    char msg[256];

    for (size_t c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        size_t stride = 0, slot0 = 0, objs = 0, slack = 0;
        check(rb_pool_geometry(cases[c].obj_size, &stride, &slot0, &objs, &slack),
              "[pool: poison] geometry accepts the object size");
        snprintf(msg, sizeof msg, "[pool: poison] obj_size %zu has stride %zu",
                 cases[c].obj_size, cases[c].stride);
        check(stride == cases[c].stride, msg);

        rb_pool_t *p = pool_create(cases[c].obj_size);
        check(p != NULL, "[pool: poison] pool_create succeeds");
        if (p == NULL) continue;

        /* Two slots, so the second free writes a NON-NULL link and the test
         * can tell poison-then-link from link-then-poison: if the poison
         * landed last, the link would be 0xDD bytes and reuse would not
         * return these two slots in order. */
        unsigned char *a = pool_alloc(p);
        unsigned char *b = pool_alloc(p);
        check(a != NULL && b != NULL, "[pool: poison] two allocations succeed");
        if (a == NULL || b == NULL) { pool_destroy(p); continue; }

        /* Prefilled across the FULL STRIDE, not just obj_size. Prefilling only
         * the object leaves the inter-slot padding holding recycled heap
         * bytes, which in a run that has already poisoned thousands of slots
         * are frequently 0xDD themselves -- so the check below would pass on
         * leftovers rather than on this free's poisoning. Found by mutation:
         * a memset of obj_size instead of stride escaped the suite entirely
         * until this prefill covered the padding. Writing the padding is a
         * deliberate white-box reach past the obj_size the pool promises its
         * callers, legitimate here for the same reason reading a dead slot
         * below is: this test is the pool's own. */
        memset(a, 0x5A, stride);
        memset(b, 0x5A, stride);
        pool_free(p, a);
        pool_free(p, b);

        size_t unpoisoned = 0;
        for (size_t i = sizeof(void *); i < stride; i++)
            if (b[i] != 0xDD) unpoisoned++;
        snprintf(msg, sizeof msg,
                 "[pool: poison] bytes [%zu, %zu) of a dead slot are 0xDD (%zu are not)",
                 sizeof(void *), stride, unpoisoned);
        check(unpoisoned == 0, msg);

        check(pool_alloc(p) == b, "[pool: poison] the link survived the poison (LIFO reuse works)");
        check(pool_alloc(p) == a, "[pool: poison] and the slot it linked to comes back next");

        pool_destroy(p);
    }
}
#endif

/* Everything out, everything back, nothing bought in between. */
static void test_pool_free_full_cycle(void) {
    size_t cap = pool_capacity();
    rb_pool_t *p = pool_create(POOL_OBJ_SIZE);
    check(p != NULL, "[pool: cycle] pool_create succeeds");
    if (p == NULL) return;

    void *slots[POOL_TWO_SLABS];
    size_t nulls = 0;
    for (size_t i = 0; i < POOL_TWO_SLABS; i++)
        if ((slots[i] = pool_alloc(p)) == NULL) nulls++;
    check(nulls == 0, "[pool: cycle] two slabs' worth of allocations succeed");

    for (size_t i = 0; i < POOL_TWO_SLABS; i++) {
        pool_free(p, slots[i]);
        check_pool_invariant(p, cap, "cycle (per free)");
    }

    size_t slabs = 0xBAD, live = 0xBAD, free_objs = 0xBAD;
    pool_stats(p, &slabs, &live, &free_objs);
    check(slabs == 2, "[pool: cycle] freeing every slot releases no slab");
    check(live == 0, "[pool: cycle] nothing is live");
    check(free_objs == POOL_TWO_SLABS, "[pool: cycle] every slot in both slabs is reusable");
    check_pool_invariant(p, cap, "cycle (empty)");

    fault_alloc_arm(100000);
    long before = fault_alloc_total();
    void *again[POOL_TWO_SLABS];
    size_t off_set = 0;
    for (size_t i = 0; i < POOL_TWO_SLABS; i++) {
        again[i] = pool_alloc(p);
        if (again[i] == NULL || !in_set(again[i], slots, POOL_TWO_SLABS)) off_set++;
    }
    check(off_set == 0, "[pool: cycle] every slot handed out again came from the original set");
    check(fault_alloc_total() - before == 0,
          "[pool: cycle] refilling two slabs from the free list costs zero rb_malloc calls");
    fault_alloc_disarm();

    check_all_distinct(again, POOL_TWO_SLABS, "cycle");
    pool_stats(p, &slabs, &live, &free_objs);
    check(slabs == 2 && live == POOL_TWO_SLABS && free_objs == 0,
          "[pool: cycle] back to two full slabs with no third bought");
    check_pool_invariant(p, cap, "cycle (refilled)");

    pool_destroy(p);
}

/* Teardown with all three slot states present at once: live, free-listed,
 * and never carved. Leaks are adjudicated by make asan / make memcheck. */
static void test_pool_destroy_with_populated_free_list(void) {
    size_t cap = pool_capacity();
    rb_pool_t *p = pool_create(POOL_OBJ_SIZE);
    check(p != NULL, "[pool: destroy-mixed] pool_create succeeds");
    if (p == NULL) return;

    void *slots[POOL_TWO_SLABS];
    size_t nulls = 0;
    for (size_t i = 0; i < cap + 10; i++) /* one full slab, ten slots into the next */
        if ((slots[i] = pool_alloc(p)) == NULL) nulls++;
    check(nulls == 0, "[pool: destroy-mixed] allocations across two slabs succeed");

    for (size_t i = 0; i < 20; i++) pool_free(p, slots[i]);

    size_t slabs = 0xBAD, live = 0xBAD, free_objs = 0xBAD;
    pool_stats(p, &slabs, &live, &free_objs);
    check(slabs == 2 && live == cap + 10 - 20,
          "[pool: destroy-mixed] live, free-listed and uncarved slots all present");
    check_pool_invariant(p, cap, "destroy-mixed");

    pool_destroy(p);
    check(1, "[pool: destroy-mixed] pool_destroy over a populated free list did not crash");
}
#endif

int main(void) {
    test_create_destroy_empty();
    test_destroy_null_is_safe();
    test_insert_and_find_single();
    test_insert_and_find_multiple();
    test_find_missing_key_returns_null();
    test_overwrite_frees_old_value();
    test_overwrite_with_null_value_free_does_not_crash();
    test_foreach_visits_in_order();
    test_validate_on_ascending_insertion_order();
    test_validate_on_descending_insertion_order();
    test_validate_on_scrambled_insertion_order();
    test_validate_on_empty_tree();
#ifdef RBTREE_TEST_HOOKS
    test_validate_detects_red_root();
    test_validate_detects_red_red();
    test_validate_detects_black_height_mismatch();
    test_validate_detects_order_violation();
    test_validate_detects_size_mismatch();
#endif
    test_insert_single_root_is_black();
    test_insert_ascending_triggers_rr_rotation();
    test_insert_descending_triggers_ll_rotation();
    test_insert_triggers_red_uncle_recolor();
    test_insert_triggers_lr_triangle_double_rotation();
    test_insert_triggers_rl_triangle_double_rotation();
    test_destroy_single_node();
    test_destroy_left_heavy_chain();
    test_destroy_calls_value_free_once_per_node();
    test_delete_table_driven();
    test_delete_missing_key_returns_error();
    test_delete_frees_value();
    test_fault_rb_create_failure();
    test_fault_insert_node_alloc_failure();
    test_fault_insert_key_alloc_failure();
    test_fault_overwrite_allocates_nothing();
    test_fault_delete_allocates_nothing();
    test_fault_sweep();
#ifdef RBTREE_TEST_HOOKS
    test_pool_geometry_table();
    test_pool_create_empty_stats();
    test_pool_create_rejects_unusable_obj_size();
    test_pool_create_allocation_failure();
    test_pool_create_destroy_is_one_allocation();
    test_pool_lifecycle_at_geometry_extremes();
    test_pool_alloc_first_slot();
    test_pool_alloc_fills_one_slab_then_grows();
    test_pool_alloc_one_malloc_per_slab();
    test_pool_alloc_slots_do_not_overlap();
    test_pool_alloc_single_slot_geometry();
    test_pool_alloc_grow_failure_first_slab();
    test_pool_alloc_grow_failure_later_slab();
    test_pool_alloc_stride_is_not_obj_size();
    test_pool_destroy_with_live_objects();
    test_pool_free_one_slot();
    test_pool_free_reuse_before_carving();
    test_pool_free_is_lifo();
    test_pool_free_reuse_costs_no_allocation();
    test_pool_free_spills_to_carving();
    test_pool_stats_free_objs_counts_both_terms();
#ifndef NDEBUG
    test_pool_free_poisons_dead_slot();
#endif
    test_pool_free_full_cycle();
    test_pool_destroy_with_populated_free_list();
#endif
    printf(failures == 0 ? "\nAll tests passed.\n" : "\n%d test(s) FAILED.\n", failures);
    return failures == 0 ? 0 : 1;
}
