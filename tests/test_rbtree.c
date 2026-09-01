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
    test_destroy_single_node();
    test_destroy_left_heavy_chain();
    test_destroy_calls_value_free_once_per_node();
    printf(failures == 0 ? "\nAll tests passed.\n" : "\n%d test(s) FAILED.\n", failures);
    return failures == 0 ? 0 : 1;
}
