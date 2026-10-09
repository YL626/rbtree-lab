# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project status

This is a CS370 lab implementing a red-black tree in C, now on its second assignment (HW2,
`CS370-HW2.pdf`, a continuation of HW1). `include/rbtree.h` is the frozen contract — it was
re-frozen for HW2 and now also declares `rb_create_pooled` (Mutation 2: node storage drawn
from an internal slab pool) and `rb_snapshot` (Mutation 4: O(1) copy-on-write snapshot, "The
Reach" — no longer just ungraded prep, see below). `rb_create_pooled` is now implemented
(commit `3d1afa9`); `rb_snapshot` is not. `NOTES.md`
contains the assignment spec, the design decisions already made (including a devlog of the
NIL/parent-pointer/teardown reasoning and an HW2 section with spec notes/tips), and
constraints that must not be violated — read it before writing any code here.

Current state of `src/rbtree.c`: `rb_create`, the node/tree structs (with parent pointer),
the `rb_malloc`/`rb_free` seam, `rb_find` (real BST search), `rb_size`, `rb_insert` (with
rotations and insertion fixup, all cases + mirrors), `rb_foreach` (recursive in-order),
`rb_validate` (all five invariants), and `rb_destroy` are implemented. `rb_destroy` already
uses the non-recursive "Reach" technique (rotate into a right spine while freeing) — see
"The Reach" below; it is not just the ungraded future work it's described as there.
**`rb_delete` is now implemented**, including `delete_fixup` (the doubly-black fixup loop,
all cases plus the mirrored right-child symmetry) — both live in `src/rbtree.c` as of the
M2 commits (`f32e9ef`..`8c55919`). `tests/fault_alloc.c` is implemented as of HW2's M1
commits (`a250531`, `a8f4439`).

**`src/pool.c` is now complete** across four committed slices: the slot geometry
(`febfdc2`), the pool lifecycle with a lazy first slab (`d1ab2d2`), slab growth and carving
behind `pool_alloc` (`0ebee46`), and the intrusive free list with `pool_free` plus 0xDD debug
poisoning (`21a44be`). **Pooled-tree integration is also done** (`3d1afa9`, `36f82d1`):
`struct rbtree` carries an `rb_pool_t *pool` whose NULL-ness *is* the mode discriminator, and
every mode-dependent decision in `src/rbtree.c` lives in exactly three places — the
`node_alloc`/`node_release` pair (the single point where node storage is obtained and
returned, which is what keeps `rb_insert`/`rb_delete` free of allocator branches),
`rb_create_pooled`, and `rb_destroy`'s teardown branch. Only *node* storage is pooled; key
copies are variable-length and stay on the `rb_malloc`/`rb_free` seam.

Pooled teardown deliberately does **not** `pool_free` each node: whole-tree destruction is
what `pool_destroy` is for (it releases every slab whether its slots were live, free-listed
or never carved), so the walk exists only to free the keys and owned values, which live
*outside* the slabs. Ordering is load-bearing — capture `next`, free the key, call
`value_free`, (ordinary mode only) `rb_free` the node; then `pool_destroy`, then the handle.
Capturing `next` first matters more under the pool than without it, since a pooled node is
still live slab memory after teardown passes over it, so a stale read would be invisible to
ASan rather than a reported use-after-free. One consequence to keep in mind: because teardown
never walks `live` down to zero, `live + free_objs == slabs * objs_per_slab` is never checked
against a *tree-driven* pool — a node skipped by teardown is caught via its leaked key under
valgrind, which is sound but indirect. There is deliberately no `rb_test_pool_stats` hook.

Still unimplemented: `rb_snapshot` (Mutation 4) and Mutation 3's O(1)-auxiliary-space
`rb_foreach` (currently recursive, which HW1 §9 permits).

`tests/fuzz.c` runs insert/find/delete ops against a reference-model oracle (an array of
key/value pairs), interleaving `rb_delete` per its later M2 commit. Its usage is now
`fuzz <op_count> [seed|-] [heap|pooled]` — the mode selects the constructor, and `-` keeps the
default `time(NULL)` seeding so a mode can be passed positionally without pinning the seed. `tests/test_rbtree.c`
covers insert/find (including overwrite and missing-key cases), foreach ordering, validate
(ascending/descending/scrambled/empty), the insertion-fixup rotation cases (LL/RR/LR/RL,
red-uncle recolor), destroy (single node, chain, value_free-once), and the table-driven
`rb_delete` cases. This now spans 17 rows in `delete_cases[]`: the spec's required minimum
(red leaf + mirror, black leaf with red sibling + mirror, two-children root/non-root,
single-node root, black node with one red child + mirror — 9 rows) plus an M2 coverage-audit
pass that pins every remaining named `delete_fixup` branch by construction rather than
relying only on the fuzzer to hit them incidentally: Case 4 far-nephew-red-direct + mirror,
Case 4 near-nephew-red-conversion + mirror (the `rotate_right(t, sib)`/`rotate_left(t, sib)`
branch), Case 3a (both nephews black, red parent, terminal) + mirror, and Case 3b (both
nephews black, black parent — a genuine multi-level climb, 2 climbing passes before a Case-4
terminal) + mirror (8 rows). The Case-4/3a/3b rows needed real search to construct (exhaustive
permutation search for Case 3a — no tree under 8 nodes produces it; randomized search +
delta-debug shrinking for Case 3b's multi-level climb — no tree under 38 nodes reproduces two
real climbing passes), not hand-derivation, and every sequence was verified against the real
compiled `rb_delete`/`delete_fixup` (an instrumented scratch copy for tracing, never a
reimplementation) before being added. A genuine climb-to-root case was searched for (random
trees up to 500 nodes, plus a targeted search for all-black-ancestor-chain leaves) and not
found; per NOTES.md's own "Confusions" note this gap is deliberately left to the fuzzer's
existing ≥10^5-op coverage rather than forced into a table row — the spec's table-driven
requirement (below) does not name it, and its absence from the table is a scoped decision, not
an oversight. See `PROMPTLOG.md` episode 2 for the full derivation-and-pushback record.

As of 2026-10-08: `make test`, `make asan`, and `make memcheck` (from a clean rebuild) all
pass with zero findings — 1897 unit assertions, the fuzzer at 10^5 ops in *both* node-storage
modes under test/asan and 2×10^4 ops in both under valgrind (three valgrind invocations, each
reporting "All heap blocks were freed" and 0 errors). HW1's baseline was the same three gates
green at 2026-09-15; the pool allocator, fault injection and pooled integration have been
added without regressing it. Mutation 3 (snapshot/Reach, non-recursive `rb_foreach`) starts
from here and must not regress it either.

Coverage of the two-mode suite is a checked fact, not an inference: the heap pass emits 637
labelled assertions and the pooled pass 613, and diffing the two description sets with the
sweep excluded is identical — all 565 non-sweep tree assertions run in both modes, in the same
order. The 24-line delta is sweep arithmetic (heap allocates 1 + 3×2 = 7 and terminates at
n = 8; pooled allocates 2 + 2 + 1 + 1 = 6 and terminates at n = 7), plus five `make_int`
fixture checks firing from inside the sweep. If a future change makes those two counts move
for any other reason, that is a test silently degrading into a no-op and must be investigated,
not re-baselined.

## Code map

- `include/rbtree.h` — frozen public API / graded contract. Never edit.
- `src/rbtree.c` — tree logic: allocation/ownership, rotations, insertion and deletion
  fixups, `rb_validate`, teardown. All heap allocation here goes through the `rb_malloc`/
  `rb_free` seam.
- `src/pool.c` — HW2 slab pool implementation (backs `rb_create_pooled`); complete.
  `rb_pool_geometry` (stride, first-slot offset, slots per slab, tail slack) and the in-band
  `struct rb_slab` header whose size it accounts for; `struct rb_pool`; `pool_create` (lazy —
  buys no slab, so it has one fallible step and no unwind path), `pool_alloc` (free list →
  uncarved → grow, the grow being the only fallible step and positioned last so "leaves the
  pool as it was" holds by position rather than by undo), `pool_free` (intrusive free list,
  0xDD-poisoning the full stride first so the link is not overwritten by the poison),
  `pool_stats` (maintained counters plus a bounded debug-only free-list cross-check, so the
  headline invariant is not checked solely against the numbers the mutators just wrote), and
  `pool_destroy` (saves `slab->next` before each `rb_free`, since the chain link is in-band).
  Internal to the file by design — no `src/pool.h`, since the spec's file map is fixed.
  `tests/test_rbtree.c` declares what it needs itself under `RBTREE_TEST_HOOKS` (with
  `rb_pool_t` left incomplete), as it already does for the `rb_test_*` hooks; `src/rbtree.c`
  declares the four entry points it calls the same way.
- `tests/test_rbtree.c` — unit tests / HW1 regression suite (table-driven `rb_delete` cases,
  insertion-fixup cases, validate, destroy, overwrite semantics), plus the pool-internal
  tests. Structured as `run_tree_suite()` run twice via the `make_tree` factory — once with
  `rb_create`, once with `rb_create_pooled` — so the semantic assertions are written once and
  executed against both allocators. `check()` prefixes `mode_label` so a failure names its
  mode. Three fault tests deliberately keep naming `rb_create`: `fault_alloc_arm(1/2/3)` pins
  `src/rbtree.c`'s own ordinals (handle, node struct, key copy), which under the pool are
  different allocations in different numbers, so a pooled run of them would not be the same
  claim — their pooled counterparts are separate tests. Pool-internal tests are not tree tests
  and stay outside the two-mode suite. When adding a tree test, use `make_tree`, not a
  constructor by name.
- `tests/fuzz.c` — randomized stress driver / reference-model (oracle) testing against a
  plain array of key/value pairs. Takes `<op_count> [seed|-] [heap|pooled]`.
- `tests/fault_alloc.c` / `tests/fault_alloc.h` — allocation-seam shim and fault-injection
  support for HW2 (make allocation fail on demand). Implemented: `rb_malloc`/`rb_free` plus
  `fault_alloc_arm`/`fault_alloc_disarm`/`fault_alloc_total`.

## Build and test commands

```sh
make          # builds build/test_rbtree and build/fuzz
make test     # builds, then runs ./build/test_rbtree, then the fuzzer at 100000 ops
              # in BOTH modes: `fuzz 100000 - heap` && `fuzz 100000 - pooled`
make asan     # rebuilds with -fsanitize=address,undefined, then runs `test`
make memcheck # builds, then runs three binaries under valgrind --leak-check=full
              # (test_rbtree; fuzz 20000 heap; fuzz 20000 pooled), --error-exitcode=1
make clean
```

There is no test filter — `test_rbtree` is a single binary of unit tests (including the
table-driven `rb_delete` cases, run once per node-storage mode) invoked as
`./build/test_rbtree`; the fuzzer takes `<op_count> [seed|-] [heap|pooled]`
(`./build/fuzz 100000 - pooled`, or `./build/fuzz 100000 42 heap` to pin a seed for a
reproducible run). Both fuzz modes must pass. Do not reduce an op count to save runtime —
NOTES.md's own note on this permits moving a *large* case to its own target, not shrinking
one quietly. Toolchain is `gcc -std=c23 -Wall -Wextra -Werror`.
Every change must be clean under both `make asan` and `make memcheck` before it's considered
done — the spec requires zero findings across the whole suite, including the empty-tree case.

## Architecture (per NOTES.md / include/rbtree.h)

- **Ownership contract**: keys are C strings, always copied and owned by the tree. Values
  are opaque `void *`; the tree takes ownership of a value only on a *successful* `rb_insert`
  and releases it via the `value_free` destructor (from `rb_create`) on delete/destroy/overwrite.
  On insert failure the tree is unchanged and the caller keeps ownership of `value`. Overwriting
  an existing key frees the old value (the tree is its sole owner at that instant) before
  installing the new one. Every allocation has exactly one owner at all times.
- **Allocation seam (now load-bearing, HW2)**: ALL heap allocation in `src/` must route
  through the `rb_malloc`/`rb_free` seam — direct `malloc`/`free` anywhere in `src/` is a
  defect, not a style nit. The seam exists so production allocation, the fault injector
  (`tests/fault_alloc.c`/`.h`), and the HW2 pool allocator (`src/pool.c`) can each sit
  underneath the tree interchangeably, without `rbtree.c`'s logic depending on which
  implementation is currently active.
- **Allocation failure (HW2)**: any allocation may fail, and every call through the seam
  must be checked. Every failure path must unwind completely with no leaks — use the
  goto-cleanup pattern below. A failed operation must leave the tree in exactly the state it
  was in before the call, and must return its documented error code. Caller-owned values
  (e.g. the `value` passed to `rb_insert`) must remain caller-owned when an operation fails;
  the tree must never free or take ownership of something it didn't successfully absorb.
- **Node shape**: nodes carry a parent pointer (a deliberate design choice beyond the bare
  minimum). NIL is represented as `NULL`, not a shared sentinel object — a shared sentinel
  is incompatible with per-node parent pointers (every node with a missing child would claim
  to be that one sentinel's parent). Every place that needs a node's color must route through
  a helper that treats `NULL` as black, never dereference `->color` on a pointer that might
  be `NULL`. Full reasoning in NOTES.md's devlog. Current, actual shape (matches
  `src/rbtree.c`):
  ```c
  typedef enum { RED, BLACK } rb_color_t;
  struct rb_node {
      char           *key;    /* heap copy; the tree owns it */
      void           *value;  /* ownership per the header contract */
      struct rb_node *parent; /* NULL for the root */
      struct rb_node *left;
      struct rb_node *right;
      rb_color_t      color;
  };
  ```

- **Rotations own root maintenance**: `rb_validate` must never be responsible for fixing up
  `t->root` — rotations themselves must keep it correct.
- **Deletion fixup**: implement all cases, including deleting a black node with exactly one
  child, and the mirrored (right-child-of-right-child) symmetry of every fixup case — the
  spec explicitly calls out the mirror image as "the half people forget."
- **`rb_foreach` / `rb_destroy`**: recursion is permitted for this assignment (spec §9). See
  "The Reach" below for the ungraded non-recursive teardown.
- **`rb_validate` invariants** (must detect and report *which* invariant broke):
  1. root is black
  2. no red node has a red child
  3. every root-to-NIL path has the same black-height
  4. in-order traversal is strictly increasing under `strcmp`
  5. `rb_size` matches the actual node count
- **Test coverage requirements**: table-driven `rb_delete` tests covering at minimum a red
  leaf, a black leaf with a red sibling, a node with two children, and root deletion — each
  asserting `rb_validate` and `rb_size` afterward. The fuzzer (`tests/fuzz.c`) must run ≥ 10^5
  random insert/find/delete ops against a reference model (sorted array or linked list is
  fine), calling `rb_validate` at least every 100 ops, and must be clean under both asan and
  memcheck.

## The Reach — HW1 (ungraded) vs HW2 (`rb_snapshot`, in scope)

HW1 framed non-recursive, O(1)-auxiliary-space teardown as ungraded prep for "the next
assignment" (spec §10, "The Reach") — that next assignment is this one. `rb_destroy`'s
rotate-into-a-right-spine-while-freeing technique already exists in `src/rbtree.c` from HW1;
HW2's frozen header now adds `rb_snapshot` (Mutation 4: O(1) copy-on-write snapshot, NULL on
allocation failure), which is graded here and is not yet implemented. Treat any reuse of the
Reach technique for `rb_snapshot` as a new, from-scratch design question, not a copy-paste of
`rb_destroy` — check CS370-HW2.pdf's own section for `rb_snapshot` before attempting it, and
CS370-HW1.pdf §10 for the original Reach writeup.

## Process requirements / deliverables (spec §7, §15, §17)

These are graded independently of the code and must not be neglected or bulk-generated at the end:

- **Git history**: ≥ 8 meaningful commits tracking the milestones (M0–M3 in the schedule table,
  spec §5 — `M<n>: <what>` in a commit message names the milestone, not a "Mutation"). A single
  "final submission" commit is an automatic process-grade of zero.
- **`PROMPTLOG.md`**: 6–10 annotated episodes (prompt, what came back, your judgment — accepted,
  rejected, or pushed back on, and why). Must include at least: one plan you revised, one
  rejected/oversized diff, one tool-output debugging loop (e.g. a valgrind/asan trace fed back
  verbatim), and one adversarial-review finding you triaged. Raw transcript pastes score nothing —
  the annotation is the deliverable.
- **`REFLECTION.md`** (~1 page): where the agent was most/least reliable, one bug it introduced
  that you caught and how, and what most surprised you as a C newcomer.
- **Raw session transcripts**: the actual `.jsonl` files under `~/.claude/projects/<project>/`,
  copied in as-is — not a summary, not `/export`, not retyped. Work in this project's own
  dedicated directory so the copy is a single `cp` and doesn't pull in unrelated sessions.
  Transcripts auto-purge after 30 days by default — copy them out periodically.
- Grading corroborates these against the repo (commits, diffs, test files, line numbers), not
  against the log's own narrative — a claim with no matching artifact doesn't count.

## Working style noted in NOTES.md

# rbtree-lab: project rules
## Commands
- Build & unit tests: ‘make test‘
- Sanitizers: ‘make asan‘ Valgrind: ‘make memcheck‘
- A change is DONE only when all three pass. Always run them; show output.
## Hard constraints
- NEVER modify include/rbtree.h. It is the graded contract (HW1 and HW2 alike).
- All heap allocation in src/ must go through rb_malloc/rb_free. Direct
malloc/free in src/ is a defect — the seam must stay the single point where
production allocation, the fault injector, and the pool allocator can swap in.
- Check every allocation. Any allocation may fail; a NULL/failure return must
leave the tree unchanged, fully unwind with no leaks, leave caller-owned
values caller-owned, and return the documented error code.
- NEVER weaken, skip, or delete a test to make the suite pass. If a test
looks wrong, stop and explain why instead.
## Style
- C23. -Wall -Wextra -Werror must stay clean. No VLAs.
- goto is allowed only for cleanup-label unwinding (goto-cleanup pattern for
multi-allocation / multi-failure-path functions).
- Prefer the smallest diff that passes. Do not refactor unrelated code.
- Every non-obvious loop gets a one-line invariant comment.
## Workflow
- For any multi-file or algorithmic change: propose a plan and wait for
approval before editing.
- Commit only from a green state; message format "M<n>: <what>".


Plan first, keep diffs small, verify everything with the build tools above, and don't merge
anything that can't be explained.
