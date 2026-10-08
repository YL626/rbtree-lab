/* Mutation 2: slab pool allocator.
 *
 * Four slices live here so far: the slot geometry (the arithmetic that
 * divides one page-sized slab into aligned, fixed-size slots), the pool
 * lifecycle (pool_create / pool_stats / pool_destroy), slab growth plus slot
 * carving behind pool_alloc, and the intrusive free list behind pool_free
 * with its 0xDD debug poisoning. The allocator is now complete; what remains
 * for later slices is rb_create_pooled and the stress/fault battery over it.
 *
 * Design decisions this encodes (full reasoning in NOTES.md's HW2 "Choices
 * left up to me" and DEVLOG.md 2026-10-06/2026-10-08):
 *
 *   - Slots are aligned to alignof(max_align_t), i.e. fundamental alignment.
 *     A slot's bytes are used two ways across its lifetime -- the live object
 *     while it lives, and the intrusive free-list "next" pointer written over
 *     its front once it dies -- and the alignment has to be legal for both.
 *     pool_create takes only obj_size and so cannot be told what its caller
 *     needs, which is why the promise is made unilaterally and conservatively.
 *     This is NOT support for over-aligned types (_Alignas beyond
 *     max_align_t); those are outside this pool's contract.
 *
 *   - Slab metadata is in-band at the front of the slab, so a slab costs
 *     exactly one rb_malloc and growing the pool has exactly one failure
 *     point. The price is that usable space is 4096 minus the header and its
 *     alignment padding, and objs_per_slab must be derived from what is left
 *     -- not from 4096, which is the off-by-one this file exists to get right.
 */
#include "fault_alloc.h"
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* In-band slab header. Only the slab chain lives here, so that a later
 * pool_destroy can walk every slab and release it; the carve cursor belongs
 * to the pool itself, which only ever carves from the newest slab. */
struct rb_slab {
    struct rb_slab *next;
};

#define RB_SLAB_BYTES ((size_t)4096)
#define RB_POOL_ALIGN  alignof(max_align_t)

/* The first slot starts after the header, rounded up to RB_POOL_ALIGN. Since
 * the stride is also a multiple of RB_POOL_ALIGN and the block rb_malloc
 * returns is already suitably aligned, EVERY slot address
 * (base + RB_POOL_SLOT0_OFFSET + i*stride) is aligned -- not merely slot 0,
 * which is the version of this bug that works until slot 1. */
#define RB_POOL_SLOT0_OFFSET \
    (((sizeof(struct rb_slab) + RB_POOL_ALIGN - 1) / RB_POOL_ALIGN) * RB_POOL_ALIGN)

static_assert((RB_POOL_ALIGN & (RB_POOL_ALIGN - 1)) == 0,
              "slot alignment must be a power of two");
static_assert(RB_POOL_SLOT0_OFFSET < RB_SLAB_BYTES,
              "the in-band slab header must leave room for at least one slot");

/* Slot geometry for a pool of obj_size-byte objects in one RB_SLAB_BYTES slab:
 * reports the slot stride, the offset of the first slot, how many whole slots
 * fit, and the trailing slack too small to hold another slot (which must
 * never be handed out).
 *
 * Internal to the pool on purpose. It is absent from the frozen
 * include/rbtree.h and deliberately has no header of its own; the file-level
 * map stays as the spec draws it, and tests/test_rbtree.c declares this
 * itself under RBTREE_TEST_HOOKS, matching the existing rb_test_* precedent.
 * Out-parameters rather than a shared struct for the same reason: no type has
 * to be visible outside this file. External linkage (rather than static) both
 * keeps the production build warning-clean while pool_create does not exist
 * yet and is what the later slices will call.
 *
 * Returns false -- with all four out-parameters zeroed -- when no complete
 * slot fits: obj_size == 0, or an obj_size too large for the slab. The
 * oversize test runs BEFORE any rounding, so aligning can never wrap; a
 * wrapped stride would otherwise report slots that do not exist, which is
 * exactly what SIZE_MAX would produce. This is an internal contract only:
 * what pool_create should do with a rejected obj_size is a later decision.
 */
bool rb_pool_geometry(size_t obj_size, size_t *stride_out, size_t *slot0_offset_out,
                      size_t *objs_per_slab_out, size_t *slack_out)
{
    *stride_out        = 0;
    *slot0_offset_out  = 0;
    *objs_per_slab_out = 0;
    *slack_out         = 0;

    if (obj_size == 0) return false;

    /* A dead slot still has to hold the intrusive free-list "next" pointer,
     * so a slot is never smaller than a pointer however small the object is. */
    size_t need   = (obj_size < sizeof(void *)) ? sizeof(void *) : obj_size;
    size_t usable = RB_SLAB_BYTES - RB_POOL_SLOT0_OFFSET;

    /* Rejected before rounding: need + RB_POOL_ALIGN - 1 wraps for sizes near
     * SIZE_MAX, and a wrapped stride reports capacity that does not exist. */
    if (need > usable) return false;

    size_t stride = ((need + RB_POOL_ALIGN - 1) / RB_POOL_ALIGN) * RB_POOL_ALIGN;

    /* Unreachable while usable is itself a multiple of RB_POOL_ALIGN (rounding
     * a value <= usable up cannot then exceed it), but kept so that changing
     * the header size can only cost capacity, never produce a bogus slot. */
    size_t objs = usable / stride;
    if (objs == 0) return false;

    *stride_out        = stride;
    *slot0_offset_out  = RB_POOL_SLOT0_OFFSET;
    *objs_per_slab_out = objs;
    *slack_out         = usable - objs * stride;
    return true;
}

/* ------------------------------------------------------------------------
 * SLICE A: pool_create / pool_stats / pool_destroy.
 *
 * Still absent, deliberately: pool_alloc, pool_free, slab carving, the
 * intrusive free list, 0xDD debug poisoning, the debug free-list cross-check
 * walk, and rb_create_pooled. A pool built here therefore owns no slabs and
 * can hand out nothing; what it does is cache its geometry, report honest
 * zeroes, and tear down.
 *
 * Decisions this encodes (reasoning in NOTES.md / DEVLOG.md 2026-10-08):
 *
 *   - LAZY first slab. pool_create buys no slab, so it has exactly one
 *     fallible allocation and therefore no unwind path at all. The "no
 *     capacity, grow" branch pool_alloc needs for slab 2 is the same branch
 *     it needs for slab 1, so eager allocation would have bought nothing in
 *     alloc-side simplicity in exchange for a second failure path here. It
 *     also moves first-slab-allocation-failure to a stronger test site: a
 *     pool_alloc that must return NULL, leave every counter untouched, and
 *     still work once the injector is disarmed.
 *
 *   - Geometry is computed ONCE, here, and cached. pool_alloc/pool_free must
 *     be O(1) and must not divide or round; and rb_pool_geometry's false
 *     return gets checked at exactly one site in the program.
 *
 *   - Preconditions, not guarantees: pool_stats and pool_destroy require a
 *     non-NULL pool, and pool_stats requires all three out-parameters. These
 *     are asserted, not branched on -- the pool is internal to this file and
 *     owes callers no NULL-tolerance that the assignment does not ask for.
 */

typedef struct rb_pool rb_pool_t;

struct rb_pool {
    /* Geometry, fixed at pool_create and read-only afterwards. objs_per_slab
     * is stored rather than recomputed because it is the right-hand side of
     * the live + free_objs == slabs * objs_per_slab invariant: re-deriving it
     * at check time would let a geometry bug cancel out of its own check. */
    size_t obj_size;
    size_t stride;
    size_t slot0_offset;
    size_t objs_per_slab;

    /* Slab chain, newest first. Only pool_destroy reads it; nothing on the
     * alloc/free path needs to know which slab a slot came from, which is
     * what keeps those paths O(1). */
    struct rb_slab *slabs;
    size_t          slab_count;

    /* Carve state. One cursor pair is enough because every slab except the
     * head is fully carved -- growth happens only when carve_remaining == 0.
     * Growing earlier would strand the old head's uncarved slots, which is
     * exactly the "slot lost on the floor" the invariant exists to catch. */
    unsigned char *carve_next;
    size_t         carve_remaining;

    /* Free list of dead slots, threaded through the slots themselves.
     * free_count is the list's length only; free_objs as the spec defines it
     * (what pool_alloc can return without calling rb_malloc) is
     * free_count + carve_remaining, summed in pool_stats. */
    void   *free_head;
    size_t  free_count;

    size_t live;
};

/* Returns NULL if obj_size admits no complete slot (see rb_pool_geometry) or
 * if the pool struct itself cannot be allocated. The geometry test runs
 * BEFORE the allocation, so a rejected obj_size costs no rb_malloc call at
 * all and there is nothing to release on that path. */
rb_pool_t *pool_create(size_t obj_size)
{
    size_t stride, slot0_offset, objs_per_slab, slack;

    if (!rb_pool_geometry(obj_size, &stride, &slot0_offset, &objs_per_slab, &slack))
        return NULL;

    rb_pool_t *p = rb_malloc(sizeof *p);
    if (p == NULL) return NULL;

    p->obj_size      = obj_size;
    p->stride        = stride;
    p->slot0_offset  = slot0_offset;
    p->objs_per_slab = objs_per_slab;
    /* slack is geometry's fourth output and the pool has no use for it: "is
     * there room for another slot" is answered by carve_remaining, never by
     * arithmetic against the slab end. Asserted in the geometry test instead. */

    p->slabs           = NULL;
    p->slab_count      = 0;
    p->carve_next      = NULL;
    p->carve_remaining = 0;
    p->free_head       = NULL;
    p->free_count      = 0;
    p->live            = 0;

    return p;
}

/* ------------------------------------------------------------------------
 * SLICE B: slab growth and slot carving.
 *
 * Still absent: pool_free, the intrusive free list, and 0xDD debug poisoning.
 * A carved slot therefore holds whatever rb_malloc left in it and is the
 * caller's to initialize; and because nothing can be returned yet, live only
 * ever rises.
 */

/* Buys one slab and makes it the carve head. Returns false having mutated
 * NOTHING if the allocation fails -- which is what lets pool_alloc's failure
 * path be a bare `return NULL` with no unwinding to do.
 *
 * Only ever called with the head slab fully carved: growing while the old
 * head still had uncarved slots would strand them, since one cursor pair can
 * only track one slab. That is the "slot lost on the floor" the invariant
 * live + free_objs == slabs * objs_per_slab exists to catch, so it is
 * asserted here rather than left as a comment. */
static bool pool_grow(rb_pool_t *p)
{
    assert(p->carve_remaining == 0);

    struct rb_slab *slab = rb_malloc(RB_SLAB_BYTES);
    if (slab == NULL) return false;

    slab->next         = p->slabs;   /* LIFO: the newest slab is the carve head */
    p->slabs           = slab;
    p->slab_count++;
    p->carve_next      = (unsigned char *)slab + p->slot0_offset;
    p->carve_remaining = p->objs_per_slab;
    return true;
}

/* Returns one slot, or NULL if the pool must grow and cannot. O(1): no size
 * search, no coalescing, and no object->slab lookup -- every decision was
 * made once, in pool_create.
 *
 * The slot's bytes are uninitialized. p must be non-NULL.
 *
 * Order: free list, then uncarved capacity, then grow. Reuse comes first so
 * that slabs track the high-water mark of live rather than the total number
 * of operations -- carving first would leave freed slots unreachable until a
 * slab was exhausted, and an insert/delete workload would keep buying slabs
 * while holding a growing pile of reusable ones.
 *
 * The one fallible step is the grow, and it happens before any counter is
 * touched, so "a failed pool_alloc leaves the pool exactly as it was" holds
 * by position rather than by undoing anything. */
void *pool_alloc(rb_pool_t *p)
{
    assert(p != NULL);

    if (p->free_head != NULL) {
        /* The dead slot's first bytes are the link, not an object; read it
         * out before the slot is handed back to a caller who will overwrite
         * it. memcpy rather than a cast, for the reason pool_free gives. */
        void *slot = p->free_head;
        void *next;
        memcpy(&next, slot, sizeof next);

        p->free_head = next;
        p->free_count--;
        p->live++;

        /* The slot still carries its stale link and its 0xDD tail. Clearing
         * them is not this function's job: a carved slot is uninitialized
         * too, and a caller that forgets to initialize should find obvious
         * garbage rather than plausible zeroes. */
        return slot;
    }

    if (p->carve_remaining == 0 && !pool_grow(p))
        return NULL;

    /* Invariant: carve_next addresses an uncarved slot whenever
     * carve_remaining > 0, which the branch above has just guaranteed. The
     * advance is unconditional, so the cursor stays monotone; after the last
     * slot of a slab it holds the slab's one-past-the-end address when
     * slack == 0, which is a legal pointer value that is never dereferenced
     * because carve_remaining == 0 gates every read of it, and the next grow
     * resets it into a fresh slab. */
    void *slot = p->carve_next;
    p->carve_remaining--;
    p->carve_next += p->stride;
    p->live++;

    return slot;
}

/* ------------------------------------------------------------------------
 * SLICE C: the intrusive free list, pool_free, and 0xDD debug poisoning.
 *
 * What remains after this slice: rb_create_pooled, and re-running the whole
 * HW1 + M1 battery over a pooled tree.
 *
 * Both the poisoning and the free-list cross-check below are gated on
 * NDEBUG -- the same notion of a "debug build" that assert already uses in
 * this file, so there is no new knob and no build machinery. With the
 * Makefile never defining NDEBUG they are active in every build the project
 * actually produces (test, asan, memcheck, and the fuzzer), which is where
 * they are wanted; -DNDEBUG compiles both out for a measurement-only build.
 */

#define RB_POOL_POISON 0xDD

#ifndef NDEBUG
/* Measures the free list independently of free_count, which is the whole
 * point: the three numbers pool_stats reports are maintained counters, so
 * live + free_objs == slabs * objs_per_slab is otherwise checked against the
 * same values that pool_alloc and pool_free just produced. A double
 * pool_free, for instance, decrements live twice and increments free_count
 * twice -- the sum stays consistent while the same slot sits on the list
 * twice and the list has become cyclic.
 *
 * Returns false if the walk disagrees with free_count, or if it runs past
 * the number of slots that exist at all.
 *
 * O(free list), so this is debug-only and deliberately not on the alloc/free
 * path, which must stay O(1). */
static bool pool_free_list_agrees(const rb_pool_t *p)
{
    size_t bound = p->slab_count * p->objs_per_slab + 1;
    size_t n     = 0;
    void  *cur   = p->free_head;

    /* Invariant: n counts the slots already stepped over, and the step that
     * would push it past bound reports instead of taking it -- so a cyclic
     * list is a finite false, not an infinite walk. */
    while (cur != NULL) {
        if (++n > bound) return false;
        memcpy(&cur, cur, sizeof cur);
    }

    return n == p->free_count;
}
#endif

/* Returns one slot to the pool. O(1), and indifferent to which slab the slot
 * came from -- a single intrusive list is what buys that.
 *
 * CONTRACT, and it cannot be checked here: obj must be a pointer pool_alloc
 * returned from this pool and not already freed. Verifying either would need
 * a scan of the slabs or of the free list, which is exactly the O(1) the
 * allocator exists to provide. A violation is undefined behavior at the point
 * of the call; the debug cross-check above catches the double-free case
 * shortly afterwards, at the next pool_stats, which is the best O(1) allows.
 *
 * The link lives in the first sizeof(void *) bytes of the slot. That is legal
 * because the slot is dead (no live object's bytes are aliased), because the
 * geometry's pointer floor guarantees stride >= sizeof(void *) so it cannot
 * spill into the neighbour, and because every slot base is max_align_t
 * aligned and so aligned for void *. It is written with memcpy rather than
 * through a cast: these bytes were last written as some caller's object and
 * are now read as a void *, which is a type-punned access a cast would make
 * the compiler's business and memcpy does not. At -O1 it is one store. */
void pool_free(rb_pool_t *p, void *obj)
{
    assert(p != NULL && obj != NULL);
    assert(p->live > 0); /* nothing is live, so obj cannot have come from here */

#ifndef NDEBUG
    /* Poison FIRST, then write the link over the front. The reverse order
     * overwrites the link with 0xDD and corrupts the list -- the next
     * pool_alloc would return 0xDDDDDDDDDDDDDDDD as a slot address. The full
     * stride is poisoned, not obj_size: the inter-slot padding belongs to the
     * dead slot too, and a stale read can land in it just as easily. */
    memset(obj, RB_POOL_POISON, p->stride);
#endif

    memcpy(obj, &p->free_head, sizeof p->free_head);
    p->free_head = obj;
    p->free_count++;
    p->live--;
}

/* Reports the three numbers the M2 invariant is stated over. O(1): every one
 * is a maintained counter, or a sum of two. p and all three out-parameters
 * must be non-NULL. In a debug build it also cross-checks free_count against
 * a bounded walk of the free list, which is what keeps the invariant from
 * being a statement about its own operands. */
void pool_stats(const rb_pool_t *p, size_t *slabs_out, size_t *live_out, size_t *free_objs_out)
{
    assert(p != NULL && slabs_out != NULL && live_out != NULL && free_objs_out != NULL);
    assert(pool_free_list_agrees(p)); /* debug-only; see the comment on it */

    *slabs_out     = p->slab_count;
    *live_out      = p->live;
    *free_objs_out = p->free_count + p->carve_remaining;
}

/* Releases every slab and the pool itself. Live objects are not an obstacle:
 * the pool owns slabs, not objects, and one rb_free of a slab reclaims all of
 * its slots at once whether they were live, free-listed, or never carved.
 *
 * p must be non-NULL. Callers that may hold NULL check it themselves. */
void pool_destroy(rb_pool_t *p)
{
    assert(p != NULL);

    /* The chain link is in-band at the front of the very block being freed,
     * so next must be read before the rb_free that invalidates it -- the HW1
     * "save the pointer before you free the thing that contains it" rule, one
     * level down. Invariant: slab is a slab not yet released. */
    struct rb_slab *slab = p->slabs;
    while (slab != NULL) {
        struct rb_slab *next = slab->next;
        rb_free(slab);
        slab = next;
    }

    /* free_head and carve_next point INTO slabs and are dangling as of the
     * loop above; they are deliberately not walked or cleared. There is
     * nothing teardown needs from them, and reading one would be a real
     * use-after-free rather than a stale-slot read ASan cannot see. */

    rb_free(p);
}
