# PROMPTLOG

Annotated episodes from M2 (the `rb_delete` milestone: table-driven tests, the deletion plan,
`delete_fixup`, and the fuzzer extension). Each entry is: what I asked, what came back, and my
judgment call — accepted, revised, or rejected, and why. Corresponding commits:
`71b55ac`, `7bdf93a`, `f32e9ef`, `8fa9072`, `ddfdbf9`.

---

## 1. Plan revised — the "red sibling" test cases were mislabeled

**Prompt:** Asked Claude to trace, for the deletion plan, exactly how each of the 9
table-driven `rb_delete` cases reaches the deletion path its name claims, as part of defending
the plan before I'd approve it.

**What came back:** Claude hand-simulated `insert_fixup` against the two "black leaf with red
sibling" test sequences (`d,b,f,a,c` / `d,f,b,e,g`, from `71b55ac`) and initially concluded they
were correct. It then built an instrumented scratch copy of `rbtree.c` (dumping real node
colors) to double-check its own hand trace before committing to the claim in the plan — and the
compiled output disagreed with the hand simulation: both sequences actually produced a **black**
sibling with red nephews (Case 4), not the claimed **red** sibling (Case 5).

**Judgment: revised, accepted the correction.** Rather than silently patching the tests, Claude
flagged the discrepancy, searched (empirically, same scratch harness) for sequences that
actually produce a genuine red sibling, and proposed `c,b,d,f,g,l,n` / `p,n,l,k,c,i,e` as
replacements. I approved the swap before any test file was touched — this is the correction
committed in the "delete black leaf with red sibling" cases now in `tests/test_rbtree.c`. Worth
keeping in mind for future sessions: hand-simulating red-black rotations by eye is exactly the
kind of thing that looks right and isn't — verify structurally, don't just trust the trace.

---

## 2. Adversarial review, finding triaged (and traced back to my own error, not the design)

**Prompt:** Before letting Claude write `delete_fixup` (the doubly-black rebalancing loop —
NOTES.md's own callout that the mirror is "the half people forget"), I had it get a second
opinion: a fresh Plan sub-agent, with no memory of our conversation, reviewing the mirrored
loop design and the `(parent, x)`-pair approach (needed because this tree's NIL is `NULL`, so
`x` can't carry its own parent pointer) against the actual code and NOTES.md, independently.

**What came back:** The sub-agent confirmed the mirror was exact, `sib` provably non-NULL,
termination provable, and the `transplant`-then-`delete_fixup` ordering safe — but flagged an
apparent invariant-2 violation in the specific example tree I'd fed it for "Trace 2" (a red node
`k` with a stated red child `i`).

**Judgment: finding triaged, not accepted at face value.** Claude re-checked the flagged trace
against the *actual* committed test fixture (`p,n,l,k,c,i,e`, delete `p`) rather than the
fixture text in the review prompt, and found the real tree was valid — the invalid-looking
example was my own transcription slip when writing the sub-agent's prompt, not a bug in the
design or in the committed tests. This is exactly the outcome I wanted from an adversarial
review: a finding gets checked against ground truth (the repo), not rubber-stamped either way.

---

## 3. Tool-output debugging loop — a valgrind run that looked like a real bug, wasn't

**Prompt:** After `delete_fixup` was implemented, asked Claude to run the full battery
(`make test`, `make asan`, `make memcheck`) to confirm the "done" bar from CLAUDE.md.

**What came back:** `make memcheck`, run immediately after `make asan`, printed a suspicious
valgrind report — `ASan runtime does not come first in initial library list`, then `0 allocs, 0
frees`, `HEAP SUMMARY: in use at exit: 0 bytes in 0 blocks` — and exited nonzero. Read at face
value this looks like the program didn't even run.

**Judgment: diagnosed correctly, not treated as a real leak/crash.** Claude fed the raw output
back into its own reasoning rather than guessing, and traced it to a Makefile timestamp issue:
`make asan` had rebuilt `build/test_rbtree` linked against ASan, and `memcheck: all` doesn't
force a rebuild when the source files haven't changed, so valgrind was handed an
ASan-instrumented binary — which conflicts with valgrind's own instrumentation. Fix was
`make clean && make memcheck`, which came back fully green (232/232 and 19,842/19,842
allocs/frees across both binaries, 0 valgrind errors). I accepted the diagnosis and the fix
without asking for a second pass, since the clean-rebuild result was unambiguous.

---

## 4. Plan accepted under direct interrogation — no revision needed

**Prompt:** Across several turns, pushed on the `rb_delete` pseudocode directly rather than
just reading it: asked what happens when the successor is the immediate right child (no
intermediate parent), whether two nodes transiently alias the same key/value allocation after
the copy step, and what happens when the spliced node is the root itself.

**What came back:** Claude traced each scenario concretely against the actual `transplant`
logic (not just re-describing the pseudocode), including a worked example using the real
`d,b,f,a,c,e,g` fixture for the "no intermediate parent" case, and correctly identified the
aliasing window as real-but-harmless (guarded by the `free_own_payload` flag) rather than
denying it existed.

**Judgment: accepted as-is.** None of these questions surfaced an actual bug — each answer held
up, and I approved the pseudocode, the STEP0+1 slice boundary (deferring `delete_fixup` to a
separate approval), and the corrected test sequences from Episode 1 in the same round. This is
the "argue about it now" step NOTES.md itself calls out as worth doing before the walkthrough,
not after.

---

## 5. Re-audit requested, no discrepancies found — but a real coverage gap named instead of hidden

**Prompt:** After `delete_fixup` was implemented and committed, asked Claude to re-check its own
work: "double check against the 9 table-driven cases... if there are any discrepancies notify
me."

**What came back:** Claude didn't just re-run the existing suite (which was already green) — it
rebuilt a fresh instrumented scratch copy to independently re-verify that each of the 9 cases
exercises the exact path its name claims, and hand-verified that the two genuine `delete_fixup`
cases produce exact mirror-image trees (strong evidence against a sign error in the mirror
branch). No discrepancies were found this time. It also volunteered a residual gap: none of the
9 fixtures exercise a multi-level `delete_fixup` climb or a debt reaching the actual root, since
those only show up in trees too deep to hand-construct.

**Judgment: accepted the "no discrepancies" result, and accepted deferring the named gap to the
fuzzer** rather than asking for a 10th hand-built table case — the fuzzer extension (Episode 6)
was already the more appropriate tool for that specific gap, and NOTES.md's own note ("We
decided to extend the fuzzer to include case 3, where we need an extended fixup to climb up the
tree") confirms this was the intended path, not a shortcut.

---

## 6. Fuzzer extension — approved cleanly, explained for the walkthrough first

**Prompt:** Asked Claude to plan the `tests/fuzz.c` extension to cover `rb_delete`, and
explicitly asked it to explain the *existing* fuzzer's mechanism well enough that I could
present it in a walkthrough myself, not just describe the diff.

**What came back:** A plan that first explained the current file's structure in in walkthrough
terms (the reference-model-as-oracle design, why the model is deliberately dumb/linear, the
per-op branch and periodic `rb_validate`), then the minimal extension (`model_delete`, widening
the op choice to 3-way, mirroring the delete contract's two directions) as a small addition to
that existing pattern rather than a rewrite.

**Judgment: accepted without revision.** The explanation was accurate against the actual file
(checked against `tests/fuzz.c` directly, not just the plan's own claims), and the resulting
diff (`ddfdbf9`) is exactly what was planned — one new function, a widened branch, one new
delete branch, one changed print string. Ran clean under `make test`/`asan`/`memcheck`, plus
five extra 200k-op runs at different seeds with no failures, before committing.

---

## Note on process coverage

M2 didn't produce a genuinely **rejected** diff — every proposed change was ultimately approved,
sometimes only after Claude caught and revised its own mistake first (Episode 1) rather than me
pushing back on something it insisted was right. If a real rejection happens in M3, it belongs
here on its own terms rather than retrofitted into this log.
