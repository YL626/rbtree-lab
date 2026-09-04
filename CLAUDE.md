# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project status

This is a CS370 lab implementing a red-black tree in C. `include/rbtree.h` is the frozen
contract. `NOTES.md` contains the assignment spec, the design decisions already made
(including a devlog of the NIL/parent-pointer/teardown reasoning), and constraints that must
not be violated — read it before writing any code here.

Current state of `src/rbtree.c` (through M1): `rb_create`, `rb_size`, `rb_find` (real BST
search), `rb_insert` (rotations + fixup, all cases and mirrors), `rb_foreach` (recursive
in-order), and `rb_validate` (all five invariants, each reporting which one broke) are
implemented and exercised by both the unit tests and the fuzzer. `rb_destroy` already does
the *real* teardown, and via the non-recursive "Reach" technique (rotate-into-right-spine)
from spec §10 even though that's ungraded for this milestone set — it should still be
diffed against the fuzzer's teardown per NOTES.md before being trusted long-term.
`tests/test_rbtree.c` has 21 tests covering create/destroy, insert/find/overwrite,
`rb_foreach` ordering, `rb_validate` on ascending/descending/scrambled insertion, every
insert-fixup case (RR/LL straight-line, red-uncle recolor, LR/RL triangle) by name, and
several destroy variants (single node, left-heavy chain, per-node `value_free` count).
`tests/fuzz.c` is a real insert/find fuzzer against a reference array model, validating
sizes every op and `rb_validate` every 100 ops.

**Not yet started (next up, M2):** `rb_delete` is declared in the frozen header but has no
implementation anywhere — no stub, no deletion fixup, no table-driven delete tests, and the
fuzzer only exercises insert/find so far (no delete ops against the model). `src/pool.c` and
`tests/fault_alloc.c` remain empty — out of scope until their respective milestones/mutations.

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
  `rb_malloc`/`rb_free`, which just forward to `malloc`/`free`. This is the optional tip from
  the spec (not a hard requirement) — it pays off in the next assignment, where a test harness
  makes allocation fail on demand.
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

## The Reach (ungraded)

Non-recursive, O(1)-auxiliary-space teardown is **not required** for this assignment — it carries
no points (spec §10, "The Reach"). Recursion in `rb_foreach`/`rb_destroy` is explicitly permitted
here (spec §9); the non-recursive constraint is a preview of the *next* assignment's stack-budget
requirements, not something this milestone set is graded on. If time allows, the technique is to
rotate into a right spine while freeing so the tree's own pointer fields serve as the bookkeeping,
implemented behind a second function and checked against the fuzzer's teardown — see CS370-HW1.pdf
§10 for the full writeup before attempting it.

## Process requirements / deliverables (spec §7, §15, §17)

These are graded independently of the code and must not be neglected or bulk-generated at the end:

- **Git history**: ≥ 8 meaningful commits tracking the milestones (M0–M3 in the schedule table,
  spec §5 — `M<n>: <what>` in a commit message names the milestone, not a "Mutation"). A single
  "final submission" commit is an automatic process-grade of zero.
- **`PROMPTLOG.md`**: 4–6 annotated episodes (prompt, what came back, your judgment — accepted,
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
