/* Mutation 2: slab pool allocator.
 *
 * THIS SLICE IS SLOT GEOMETRY ONLY -- the arithmetic that divides one
 * page-sized slab into aligned, fixed-size slots. Nothing here allocates,
 * carves, hands out, poisons, or frees a slab yet: pool_create, pool_alloc,
 * pool_free, pool_stats, pool_destroy and rb_create_pooled are later slices.
 *
 * Design decisions this encodes (full reasoning in NOTES.md's HW2 "Choices
 * left up to me" and DEVLOG.md 2026-10-06/2026-10-07):
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
#include <stdbool.h>
#include <stddef.h>

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
