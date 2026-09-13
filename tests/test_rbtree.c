#include "rbtree.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

#define TEST_MAX_KEYS 8

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

/* No test exercises rb_validate actually rejecting a bad tree (invariant 4 or 5):
 * struct rb_node/struct rbtree are opaque outside src/rbtree.c, and rb_insert cannot
 * produce a mis-ordered tree or a size/count mismatch by construction. That case has
 * to wait until a real bug (most likely in fixup, later) can manufacture one, which
 * the fuzzer's periodic rb_validate calls are designed to catch when it happens. */

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

    /* Black leaf with red sibling: d,b,f,a,c inserted keeps b and f black
     * (children of black root d), with a and c red leaves under b. Deleting
     * f (a black leaf whose sibling b is red... */
    { "delete black leaf with red sibling",
      { "d", "b", "f", "a", "c" }, 5, "f" },
    { "delete black leaf with red sibling (mirror)",
      { "d", "f", "b", "e", "g" }, 5, "b" },

    /* Node with two children: root has two children; delete the root itself
     * so the successor-splice path is exercised on the two-children case
     * generally (also doubles as the "root deletion" requirement). */
    { "delete node with two children (root)",
      { "d", "b", "f", "a", "c", "e", "g" }, 7, "d" },
    { "delete node with two children (non-root)",
      { "d", "b", "f", "a", "c", "e", "g" }, 7, "f" },

    /* Root deletion, single node: the only node in the tree is the root. */
    { "delete root (single node tree)", { "only" }, 1, "only" },

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
    printf(failures == 0 ? "\nAll tests passed.\n" : "\n%d test(s) FAILED.\n", failures);
    return failures == 0 ? 0 : 1;
}
