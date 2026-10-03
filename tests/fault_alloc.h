#ifndef FAULT_ALLOC_H
#define FAULT_ALLOC_H
#include <stddef.h>

/* Mutation 1 allocation seam. rb_malloc/rb_free are declared here and
 * defined in fault_alloc.c; src/rbtree.c calls them without knowing whether
 * fault injection is armed. While disarmed they are a plain pass-through to
 * malloc/free. */
void *rb_malloc(size_t n);
void  rb_free(void *p);

/* Arms the injector: the n-th rb_malloc call counted from this call (n >= 1)
 * returns NULL instead of allocating. Resets the since-arm call counter. */
void fault_alloc_arm(long n);

/* Disarms the injector; no further call will be injected. Does not reset
 * the since-arm counter fault_alloc_total() reports. */
void fault_alloc_disarm(void);

/* Number of rb_malloc calls observed since the most recent fault_alloc_arm()
 * call, whether or not injection actually fired. */
long fault_alloc_total(void);

#endif
