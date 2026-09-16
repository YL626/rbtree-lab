## 2026-08-30 (M1 setup / study, ~40m)
STATE: repository still mostly skeleton code; `include/rbtree.h` is the frozen contract, while `src/rbtree.c`, `src/pool.c`, and the test files are effectively unimplemented. `NOTES.md` already contains the assignment/spec notes.
DID: created/updated `CLAUDE.md` with the real build/test commands, ownership contract, allocation seam, validator invariants, deletion-test requirements, and project architecture. Reviewed ownership rules and worked through the Java-to-C table from the spec without changing source.
DECIDED: treat key copying and value ownership as separate events; failed `rb_insert` leaves the tree unchanged and does not consume the caller's value. On overwrite, reuse the existing key allocation and free only the old value.
LEARNED: C pointer assignment aliases an object rather than copying it; `strcmp` is required for string contents; NULL dereference and out-of-bounds access are undefined behavior; ASan is part of the normal correctness workflow, not an optional afterthought.
NEXT FIRST STEP: build the smallest M1 implementation slices against the frozen header, starting with creation/destruction/allocation plumbing and then ordinary BST insertion/search before red-black fixup.
OPEN: `src/pool.c` and `tests/fault_alloc.c` are intentionally untouched for later milestones.

## 2026-08-31 (M1 core implementation)
STATE: basic tree infrastructure exists; red-black insertion fixup is not complete yet.
DID: implemented new-key and overwrite paths for `rb_insert`, real `rb_find`, in-order `rb_foreach`, `rb_validate` ordering/size checks, and `rb_destroy` using the non-recursive right-spine "Reach" teardown. Replaced the temporary recursive destroy bridge. Commit reports 62 assertions green under `make test`, ASan, and memcheck with balanced allocations/frees.
DECIDED: implement the Reach early even though recursion is still permitted in HW1, because Section 10 previews the next assignment's O(1)-auxiliary-space teardown requirement and the simpler pre-delete tree is a safer place to validate it.
LEARNED: ownership must survive every early-return path: node allocation and key-copy allocation are separate failure points, and a copied key must be unwound if a later allocation fails.
NEXT FIRST STEP: add rotations and insertion fixup case-by-case, including exact mirrors, then extend validation to the red-black color/black-height invariants.
OPEN: insertion balancing is still the main missing M1 correctness piece; deletion is not started.

## 2026-09-02 (M1 insertion fixup, ~1h)
STATE: ordinary BST insert/find/foreach/validate/destroy are working; insertion can still violate red-black invariants until fixup is complete.
DID: implemented left/right rotations and insertion fixup for all textbook cases plus mirrors (`4dad14e`). Studied the Java-to-C table and insertion cases interactively to verify the reasoning behind recolors, straight-line rotations, and kink-to-line conversion.
DECIDED: treat the inner-child/kink case as a conversion step, not a terminal fix; rotate the child/parent relation first, then reuse the straight-line case instead of inventing separate balancing logic.
LEARNED: the insertion cases are easier to defend as control-flow transformations: red uncle => recolor and climb; black uncle + kink => rotate into a line; black uncle + line => recolor/rotate and finish locally.
NEXT FIRST STEP: stress the completed insertion path against a deliberately simple reference model and run `rb_validate` periodically rather than relying only on hand-built examples.
OPEN: deletion remains completely absent; M1 still needs fuzz coverage and documentation/status cleanup.

## 2026-09-04 (M1 checkpoint, ~5m)
STATE: M1 is functionally complete. `rb_insert`, `rb_find`, `rb_foreach`, all five `rb_validate` invariants, and non-recursive `rb_destroy` are implemented; 21 unit tests and a 100,000-op insert/find reference-model fuzzer pass. `rb_delete` is still absent. `src/pool.c` and `tests/fault_alloc.c` remain empty.
DID: audited `CLAUDE.md` against `src/rbtree.c`, `tests/test_rbtree.c`, and `tests/fuzz.c`; corrected stale documentation that still described completed M1 functions as stubs. Confirmed `make test` green and recorded the real next milestone.
DECIDED: treat deletion as a fresh M2 surface rather than mixing it into cleanup of M1. Keep the validator and fuzzer as the safety net for every deletion slice.
LEARNED: project-status documentation can become dangerously stale during rapid implementation; future sessions should verify code/tests before trusting the status paragraph.
NEXT FIRST STEP: study deletion from the spec, then write deterministic failing `rb_delete` tests before implementing deletion logic.
OPEN: no delete tests, no `rb_delete`, and no delete operations in the fuzzer yet.

## 2026-09-12 (M2 evening 1, ~3h)
STATE: M1 is green; M2 begins with no deletion implementation. The spec requires table-driven delete coverage and mirrored doubly-black reasoning.
DID: studied the deletion cases from the spec; added the initial table-driven `rb_delete` cases; planned two-child reduction, zero/one-child splice, root splice, ownership/aliasing, and doubly-black follow-up in `PLAN.md`. Traced exactly when doomed nodes/payloads are freed and how successor payload ownership moves. Found that the original "red sibling" insertion sequences were mislabeled and identified genuinely red-sibling replacements.
DECIDED: reduce every two-child delete to physically removing the successor; allow a short, intentional aliasing window for successor key/value pointers, guarded so the successor node struct is freed without freeing the transferred payload. Keep fixup as a separately approved slice. Use `NULL` leaves plus an explicit `(x,parent)` pair rather than introducing a sentinel architecture late.
LEARNED: hand-simulating red-black insertion colors is unreliable enough to mislabel deletion fixtures; verify intended case shapes with the compiled implementation before treating a test name as proof of coverage.
NEXT FIRST STEP: correct the mislabeled red-sibling test sequences, then implement only STEP 0/1 of `rb_delete` (successor reduction + splice + one-red-child recolor) and stop at the planned slice boundary.
OPEN: doubly-black fixup is deliberately not implemented yet; the initial test table should remain partially red until that slice lands.

## 2026-09-13 (M2 evening 2, ~2h)
STATE: deletion table exists and the plan is approved. Before fixup, the intended intermediate target is 7/9 core delete cases green with the genuine red-sibling cases still failing on black-height.
DID: corrected the two mislabeled red-sibling fixtures; implemented `find_node`, `tree_min`, `transplant`, two-child successor reduction, zero/one-child splice, payload-transfer ownership via `free_own_payload`, and the one-red-child recolor. Reached the planned 7/9 state, then implemented the full doubly-black `delete_fixup` loop with all mirrored cases (`f32e9ef`, `8fa9072`). Ran the full battery; after recognizing the stale-ASan-binary/valgrind conflict, a clean rebuild passed test/ASan/memcheck.
DECIDED: Case 5 (red sibling) is a reshuffle that falls through to the black-sibling Case 3/4 decision in the same loop iteration; Case 3 is the only path that actually moves the debt upward and begins another iteration. Preserve the explicit parent pointer because `x` may be `NULL`.
LEARNED: the apparent valgrind failure after `make asan` was a tooling-state problem, not a tree bug: memcheck had inherited an ASan-linked binary. `make clean && make memcheck` is required when switching instrumentation modes if timestamps would otherwise skip the rebuild.
NEXT FIRST STEP: independently re-audit the 9 deletion fixtures against the real control flow, then extend the fuzzer to interleave delete operations against the reference model.
OPEN: deterministic tests cover the required delete scenarios, but they do not yet pin every internal Case 3/4 sub-path or a multi-level debt climb.

## 2026-09-13 (M2 evening 3, ~1h)
STATE: `rb_delete` and `delete_fixup` are complete and the required deterministic table is green. The remaining M2 work is stress coverage and evidence that the named fixtures really hit the paths they claim.
DID: re-audited all 9 deletion cases with instrumented traces and found no implementation discrepancy; recorded the residual multi-level-climb coverage gap rather than hiding it. Extended `tests/fuzz.c` with `model_delete` and a 3-way insert/find/delete mix (`ddfdbf9`), keeping the linear reference model as the oracle and periodic `rb_validate`. Full test/ASan/memcheck runs passed, plus five additional 200,000-op fuzz runs at different seeds.
DECIDED: use fuzzing for deeper/rarer fixup climbs instead of forcing a hand-built table row immediately; keep deterministic tests for known structural cases and the fuzzer for broad state-space coverage.
LEARNED: the reference model is intentionally simple because its job is independence, not speed; a dumb linear oracle is more valuable than duplicating red-black-tree logic in the test harness.
NEXT FIRST STEP: perform a branch-by-branch deterministic coverage audit of Case 4, Case 3a, and Case 3b so the fuzzer is not the only evidence those internal paths work.
OPEN: no dedicated table rows yet for far-red Case 4, near-red-to-far-red conversion, red-parent Case 3a, or multi-level Case 3b climb.

## 2026-09-14 (M2 evening 4, ~2h active split across 2 days)
STATE: deletion implementation is already complete and green; the work is now a coverage audit, not new fixup implementation. Existing docs briefly lagged behind that state.
DID: synced `CLAUDE.md` to the actual completed delete/fuzzer state and verified fresh `make test`, ASan, and memcheck. Then audited the fixup control flow against the spec, split Case 4 into two real paths (far nephew already red vs. near-red/far-black conversion), and added deterministic cases for both directions, Case 3a + mirror, and multi-level Case 3b + mirror. `delete_cases[]` grew from 9 to 17 rows. Search/instrumentation found a verified two-level debt climb; no climb-to-root fixture was proven. Full test/ASan/memcheck stayed green.
DECIDED: do not claim climb-to-root is impossible just because search did not find one. Separate empirical evidence from invariant-level proof. Leave climb-to-root to fuzz coverage because the assignment does not require a dedicated table row for it.
LEARNED: Case 3a's red parent is not recolored inside the both-nephews-black branch; the branch sets `x = parent`, the loop exits because new `x` is red, and the final `if (x) x->color = BLACK` absorbs the debt. Multi-level Case 3b simply repeats with the old parent becoming the new `x` and its parent/sibling becoming the next local frame.
NEXT FIRST STEP: run a reproducible fixed-seed 100,000-op delete-enabled fuzz workload under both ASan/UBSan and valgrind, preserving the seed/trace if anything fails.
OPEN: climb-to-root remains unpinned deterministically; it is not proven impossible. Documentation should say exactly that rather than overstate the search result.

## 2026-09-15 (M2 evening 4 closeout, ~30m)
STATE: 17 deterministic deletion cases are green; Case 4 direct/conversion, Case 3a, and multi-level Case 3b are pinned in both directions. Delete-enabled fuzzing is already implemented.
DID: ran the exact same 100,000-operation sequence with fixed seed `42` under ASan+UBSan and valgrind. `rb_validate` ran every 100 operations (1,000 checks/run). Both completed with no invariant failures, no sanitizer errors, and no leaks; valgrind reported 87,024 allocations and 87,024 frees. Updated project notes/PROMPTLOG coverage state and committed the deterministic coverage audit before the fuzz run.
DECIDED: use direct fixed-seed binary invocations for reproducibility rather than the Makefile's time-based seed. Keep sanitizer and valgrind builds separate to avoid instrumentation conflicts.
LEARNED: a green fuzz run is only reproducible evidence if the operation count, seed, validation cadence, and build flags are recorded together.
NEXT FIRST STEP: perform a fresh adversarial ownership/memory-safety review that assumes a bug exists and tries to falsify the current implementation rather than merely re-running the green suite.
OPEN: `src/pool.c` and `tests/fault_alloc.c` are still future work; validator negative/error-path testing has not yet been audited directly.

## 2026-09-15 (M2 evening 5, ~2h active)
STATE: core `rbtree.c` functionality, deletion, deterministic deletion coverage, and fixed-seed fuzzing are green. The review target is now latent correctness/memory-safety defects and weaknesses in the tests themselves.
DID: adversarially traced allocation/free/ownership paths and both fixup mirrors. Cleared the three named suspects as false positives: no successor-splice UAF, no copied-key leak on overwrite, and no unchecked `rb_malloc` return. Proved `delete_fixup`'s sibling cannot be `NULL` at a reachable dereference in a valid input tree and retracted an unnecessary empirical hedge after reviewing the proof. Found one genuine defect: every existing `rb_validate` test invocation only checks valid trees returning 0; no test deliberately violates invariants and verifies the corresponding nonzero detection path. Updated `PROMPTLOG.md` to record the real finding/false positives and committed that update as `7a6e2b7`. Later added the rationale for implementing the Reach early as the first PROMPTLOG episode; that final PROMPTLOG edit was not yet committed in the last transcript.
DECIDED: do not manufacture a memory-safety bug just to satisfy an adversarial-review requirement. Treat the validator issue as what it actually is: a real test-coverage defect, not evidence that `rb_validate` currently returns the wrong codes.
LEARNED: a validator used as the fuzzer's oracle must itself be tested on invalid inputs; otherwise both deterministic tests and fuzzing can agree on a false sense of correctness if the checker is broken. Also, once an invariant proof is complete, vague "needs fuzzing to fully falsify" hedging can make a correct argument less clear rather than safer.
NEXT FIRST STEP: design the smallest legitimate test-only mechanism for constructing intentionally invalid trees and assert `rb_validate` detects each invariant/error code without weakening encapsulation or production behavior.
OPEN: negative-path coverage for all five `rb_validate` invariants is missing; `tests/fault_alloc.c` and `src/pool.c` remain unimplemented future-milestone files; the final Reach/PROMPTLOG renumbering edit is uncommitted as of the last transcript.
