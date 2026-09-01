# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project status

This is a CS370 lab implementing a red-black tree in C. `include/rbtree.h` is the frozen
contract; `src/rbtree.c`, `src/pool.c`, and everything in `tests/` are currently empty
skeleton files waiting to be implemented. `NOTES.md` contains the assignment spec, the
design decisions already made, and constraints that must not be violated — read it before
writing any code here.

## Build and test commands

```sh
make          # builds build/test_rbtree and build/fuzz
make test     # builds, then runs ./build/test_rbtree && ./build/fuzz 100000
make asan     # rebuilds with -fsanitize=address,undefined, then runs `test`
make memcheck # builds, then runs both binaries under valgrind --leak-check=full
              # (test_rbtree, and fuzz with 20000 ops), --error-exitcode=1
make clean
```

There is no test filter — `test_rbtree` is a single binary of unit tests (including the
table-driven `rb_delete` cases) run via `./build/test_rbtree`; the fuzzer takes an operation
count as argv[1] (`./build/fuzz 100000`). Toolchain is `gcc -std=c23 -Wall -Wextra -Werror`.
Every change must be clean under both `make asan` and `make memcheck` before it's considered
done — the spec requires zero findings across the whole suite, including the empty-tree case.

## Architecture (per NOTES.md / include/rbtree.h)

- **Ownership contract**: keys are C strings, always copied and owned by the tree. Values
  are opaque `void *`; the tree takes ownership of a value only on a *successful* `rb_insert`
  and releases it via the `value_free` destructor (from `rb_create`) on delete/destroy/overwrite.
  On insert failure the tree is unchanged and the caller keeps ownership of `value`. Overwriting
  an existing key frees the old value (the tree is its sole owner at that instant) before
  installing the new one. Every allocation has exactly one owner at all times.
- **Allocation seam**: all allocation in `src/rbtree.c` routes through two 4-line wrappers,
  `rb_malloc`/`rb_free`, which just forward to `malloc`/`free`. `rb_create_pooled` is a second
  constructor whose node storage is drawn from an internal slab pool (`src/pool.c`).
- **Node shape**: nodes carry a parent pointer (a deliberate design choice beyond the bare
  minimum). Every missing child conceptually points at a black sentinel NIL leaf.
  typedef enum { RED, BLACK } rb_color_t;
  struct rb_node {
    char *key; /* heap copy; the tree owns it */
    void *value; /* ownership per the header contract */
    /* add parent pointer */
    struct rb_node *left;
    struct rb_node *right;
    rb_color_t color;
  };

- **Rotations own root maintenance**: `rb_validate` must never be responsible for fixing up
  `t->root` — rotations themselves must keep it correct.
- **Deletion fixup**: implement all cases, including deleting a black node with exactly one
  child, and the mirrored (right-child-of-right-child) symmetry of every fixup case — the
  spec explicitly calls out the mirror image as "the half people forget."
- **O(1)-space traversal/teardown**: `rb_foreach` and `rb_destroy` must run in O(1) auxiliary
  space with no recursion (this is Mutation 3; recursion is otherwise permitted this
  assignment, but the eventual target is non-recursive). The intended technique for teardown
  is rotating into a right spine while freeing, so the tree's own pointer fields serve as the
  bookkeeping instead of a call stack or heap-allocated stack — free the child pointer's
  target only after saving it, never dereference already-freed memory.
- **Mutation 4 (rb_snapshot)**: O(1) copy-on-write snapshot; returns NULL on allocation
  failure.
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

## Working style noted in NOTES.md

# rbtree-lab: project rules
## Commands
- Build & unit tests: ‘make test‘
- Sanitizers: ‘make asan‘ Valgrind: ‘make memcheck‘
- A change is DONE only when all three pass. Always run them; show output.
## Hard constraints
- NEVER modify include/rbtree.h. It is the graded contract.
- Check every allocation. malloc can return NULL; a NULL return must
leave the tree unchanged and return the documented error code.
- NEVER weaken, skip, or delete a test to make the suite pass. If a test
looks wrong, stop and explain why instead.
## Style
- C23. -Wall -Wextra -Werror must stay clean. No VLAs.
- Error handling: goto-cleanup pattern for multi-allocation functions.
- Prefer the smallest diff that passes. Do not refactor unrelated code.
- Every non-obvious loop gets a one-line invariant comment.
## Workflow
- For any multi-file or algorithmic change: propose a plan and wait for
approval before editing.
- Commit only from a green state; message format "M<n>: <what>".


Plan first, keep diffs small, verify everything with the build tools above, and don't merge
anything that can't be explained.
