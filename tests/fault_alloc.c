#include "fault_alloc.h"
#include <stdlib.h>

static long fail_at = 0;          /* 0 = disarmed, never fail */
static long calls_since_arm = 0;

void fault_alloc_arm(long n) {
    fail_at = n;
    calls_since_arm = 0;
}

void fault_alloc_disarm(void) {
    fail_at = 0;
}

long fault_alloc_total(void) {
    return calls_since_arm;
}

void *rb_malloc(size_t n) {
    calls_since_arm++;
    if (fail_at != 0 && calls_since_arm == fail_at) return NULL;
    return malloc(n);
}

void rb_free(void *p) {
    free(p);
}
