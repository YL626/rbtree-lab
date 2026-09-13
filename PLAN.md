# rb_delete implementation plan (DRAFT — not yet approved)

Status: awaiting approval. Nothing in `src/rbtree.c` has been modified. `tests/test_rbtree.c`
already has the table-driven `rb_delete` tests from the prior session, but two of them need a
key-sequence correction noted below (also awaiting approval).

## Slice boundary

This slice implements **only**:
- STEP 0: two-children -> in-order-successor reduction (copy successor's key/value into the
  doomed node's slot, then delete the successor instead).
- STEP 1: splice a node with 0 or 1 children out of the tree, including the trivial "recolor
  the surviving red child black" case.

Explicitly **deferred** to a later, separately-approved slice:
- STEP 2: the doubly-black sibling/parent rebalancing loop (NOTES.md's "the real fixup loop"),
  and its mirror. Where it would be invoked, leave a comment marking the seam — not a stub
  function, not a partial implementation.

This boundary was chosen because 7 of our 9 table-driven test cases pass with only STEP 0+1
(verified by tracing each case against the actual insertion-fixup behavior, using an
instrumented scratch copy of rbtree.c to dump real colors — not by hand simulation alone,
which already produced one wrong conclusion during this planning pass). The 2 remaining cases
genuinely require STEP 2 and are expected to fail `rb_validate` until that slice lands.

## Private helpers needed

1. `static struct rb_node *find_node(const rbtree_t *t, const char *key)`
   BST search returning the node itself (not just its value, unlike `rb_find`). Needed because
   `rb_delete` must manipulate `parent`/`left`/`right`, not just read `value`. Small,
   unavoidable duplication of `rb_find`'s loop body.

2. `static struct rb_node *tree_min(struct rb_node *n)`
   Leftmost descendant of the subtree rooted at `n`. Gives the in-order successor as
   `tree_min(z->right)`.

3. `static void transplant(rbtree_t *t, struct rb_node *u, struct rb_node *v)`
   Puts `v` in `u`'s slot (inside `u->parent`, or `t->root` if `u` is the root), and sets
   `v->parent = u->parent` when `v != NULL`. The *only* place that touches `t->root` or
   reparents nodes during delete — mirrors how `rotate_left`/`rotate_right` already own that
   job for insert ("rotations own root maintenance", extended to transplant).

No `delete_fixup` helper this slice — deferred, will mirror `insert_fixup`'s
while-loop-with-mirror-branch shape when it's built.

## `rb_delete` body (pseudocode)

```c
int rb_delete(rbtree_t *t, const char *key) {
    struct rb_node *z = find_node(t, key);
    if (z == NULL) return -1;

    struct rb_node *n;              /* the node actually unlinked */
    bool free_own_payload = true;   /* does n still own the key/value to free? */

    if (z->left != NULL && z->right != NULL) {
        struct rb_node *y = tree_min(z->right);   /* in-order successor */
        rb_free(z->key);
        if (t->value_free) t->value_free(z->value);
        z->key   = y->key;      /* relocate y's payload into z's slot */
        z->value = y->value;
        n = y;
        free_own_payload = false;   /* n's key/value now belong to z */
    } else {
        n = z;
    }

    struct rb_node *child = (n->left != NULL) ? n->left : n->right; /* <=1 child, guaranteed */
    bool n_was_black = is_black(n);
    transplant(t, n, child);

    if (n_was_black) {
        if (child != NULL) child->color = BLACK; /* STEP1: absorbs the debt locally */
        /* else: doubly-black black-leaf case -- STEP2 loop, deferred */
    }

    if (free_own_payload) {
        rb_free(n->key);
        if (t->value_free) t->value_free(n->value);
    }
    rb_free(n);
    t->size--;
    return 0;
}
```

## Traced invariants (for defense / review)

- **Two children:** `z` is never physically removed. `y = tree_min(z->right)` is found,
  its key/value are relocated into `z` (after freeing `z`'s old key/value), then `y` itself
  is unlinked via the <=1-child path. `z` keeps its original color/position permanently.

- **Successor is `z`'s immediate right child (no intermediate parent):** `y == z->right`,
  so `y->parent == z`. `transplant(t, y, child)` is driven purely by `y->parent`, so it
  patches `z->right` directly with no special case — this is *why* the "copy payload, delete
  y in place" design (already committed to in NOTES.md) avoids CLRS's `y.parent == z` special
  case entirely: we never physically relocate `y`'s node into `z`'s structural spot the way
  CLRS's transplant-of-whole-nodes approach does.

- **Successor is deeper in the right subtree:** `y`'s real parent is some `p != z`, and
  `y == p->left` always (leftmost node). `y` may have a right child `r`. `transplant(t, y, r)`
  patches `p->left`; `z`'s own pointers (including `z->right`) are untouched.

- **Which physical node is unlinked:** always `n` -- either `z` (<=1 child) or `y` (two
  children). `z` is only ever physically removed when it had <=1 child to begin with.

- **Key/value ownership timeline (two distinct free call-sites, not one):**
  - Two-children case: `z`'s *old* key/value are freed **first**, immediately upon finding
    `y`, *before* any structural change. `y`'s own original key/value are **never freed by
    this call** -- they're relabeled as `z`'s. Only `y`'s empty struct is freed
    (`free_own_payload = false` guards this).
  - <=1-child case: `n == z`'s key/value are freed **after** the structural transplant,
    immediately before the struct itself is freed.
  - Aliasing window: from the moment of `z->key = y->key; z->value = y->value;` until
    `rb_free(n)` on `y`'s struct, both `z` and `y` hold identical pointer values for
    key/value. Harmless *as pointer values*; would be a use-after-free / double-free if
    anything ever called `rb_free`/`value_free` through `y`'s copy -- which is exactly what
    `free_own_payload` prevents.

- **`t->root` / parent pointers / `t->size`:** only `transplant` touches `t->root` or
  reparents nodes (same discipline as the rotations). `t->size--` happens exactly once, after
  the physical unlink, mirroring `rb_insert`'s single `t->size++`.

- **Spliced node is the root:** two sub-cases, both via the <=1-child branch (`n == z ==
  root`); the two-children branch never produces `n == root` since `n` is always the
  successor there.
  - Single-node tree: `transplant(t, n, NULL)` sees `u->parent == NULL` -> `t->root = NULL`.
    No STEP1 recolor branch fires (`child == NULL`). Matches NOTES.md: "if the debt climbs
    all the way to the root: it just disappears" -- here it never even has to climb.
  - Root with one (necessarily red) child: `transplant(t, root, child)` sets
    `child->parent = NULL` and `t->root = child`; STEP1 then recolors `child` black,
    simultaneously restoring invariant 1 for the new root. No special-casing needed --
    `transplant`'s `u->parent == NULL` branch is the same one rotations already use.

## Table-driven test case audit (verified via instrumented scratch build, not hand simulation)

| case | structure found | outcome this slice |
|---|---|---|
| red leaf (`b,a,c`, delete `a`/`c`) | `b`(B) w/ `a`,`c`(R) leaves | passes |
| two children, root (`d,b,f,a,c,e,g`, delete `d`) | successor `e` is a red leaf | passes |
| two children, non-root (delete `f`) | successor `g` is a red leaf | passes |
| root, single node (delete `only`) | no parent, no loop needed regardless of color | passes |
| one red child, left (`b,a`, delete `b`) | `b`(B) root, `a`(R) child | passes |
| one red child, right (`a,b`, delete `a`) | mirror | passes |
| black leaf w/ red sibling (current test: `d,b,f,a,c`, delete `f`) | **mislabeled**: sibling `b` is BLACK w/ red children -> this is Case 4, not Case 5 | fails (needs STEP2 either way, but for the wrong stated reason) |
| mirror (current test: `d,f,b,e,g`, delete `b`) | same mislabeling, sibling `f` is BLACK | fails |

### Pending correction (needs sign-off before editing tests/test_rbtree.c)

Replace the two mislabeled cases with sequences verified (via instrumented scratch build) to
produce a genuinely RED immediate sibling:

- **Primary:** insert `c,b,d,f,g,l,n`; delete `b`. `b` is a black leaf, sibling `f` is RED
  (children `d` black-leaf, `l` black w/ red children `g`,`n`). Real Case 5
  (red sibling -> rotate+swap -> re-enter loop).
- **Mirror:** insert `p,n,l,k,c,i,e`; delete `p`. `p` is a black leaf, sibling `k` is RED on
  the *left*, forcing the mirrored rotation direction.

## Status of the STEP 0+1 slice above

**Approved and implemented.** `find_node`, `tree_min`, `transplant`, and `rb_delete`'s STEP0/1
body are committed (`f32e9ef`). 7 of 9 table cases pass; the two "black leaf with red sibling"
cases (now using the corrected `c,b,d,f,g,l,n`/`p,n,l,k,c,i,e` sequences, also committed) fail
`rb_validate` exactly as expected, because STEP 2 doesn't exist yet. This is the current gap.

---

# STEP 2: doubly-black fixup loop (this planning round)

## Why now

This is the only remaining gap in `rb_delete`. Without it, deleting any black leaf whose
removal doesn't get absorbed by STEP 1's trivial one-child recolor corrupts invariant 3
(black-height) -- exactly the 2 failing table cases, and (once the fuzzer is extended to call
`rb_delete`, a separate follow-up) any fuzzer run that deletes a black leaf.

## The core design problem: `x` can be NULL, and NULL can't hold its own parent

CLRS's classic delete-fixup relies on a mutable sentinel NIL node so the deficient node `x`
always has a real `x->parent` to read. This tree deliberately has **no sentinel** (NIL ==
`NULL`), specifically because a shared sentinel is incompatible with per-node parent pointers
(every node with a missing child would claim to be that one sentinel's parent) -- an explicit,
already-committed design decision from NOTES.md's devlog.

The fix: thread `x` and its parent through the loop as an explicit **pair**, `(parent, x)`,
instead of ever reading `x->parent`. `rotate_left`/`rotate_right` (already in `rbtree.c`,
lines ~51-73) are reused unchanged.

## `delete_fixup` (verified design -- independently reviewed, see below)

```c
static void delete_fixup(rbtree_t *t, struct rb_node *parent, struct rb_node *x) {
    while (parent != NULL && is_black(x)) {
        if (x == parent->left) {
            struct rb_node *sib = parent->right;
            if (is_red(sib)) {                      /* Case 5: sibling red */
                sib->color = BLACK;
                parent->color = RED;
                rotate_left(t, parent);
                sib = parent->right;
            }
            if (is_black(sib->left) && is_black(sib->right)) {  /* Case 3: both nephews black */
                sib->color = RED;
                x = parent;
                parent = x->parent;
            } else {
                if (is_black(sib->right)) {          /* near red / far black: convert first */
                    if (sib->left != NULL) sib->left->color = BLACK;
                    sib->color = RED;
                    rotate_right(t, sib);
                    sib = parent->right;
                }
                sib->color = parent->color;          /* Case 4: far nephew red, terminal */
                parent->color = BLACK;
                if (sib->right != NULL) sib->right->color = BLACK;
                rotate_left(t, parent);
                return;
            }
        } else {
            /* exact mirror: left/right swapped, rotate_right/rotate_left swapped */
            struct rb_node *sib = parent->left;
            if (is_red(sib)) {
                sib->color = BLACK;
                parent->color = RED;
                rotate_right(t, parent);
                sib = parent->left;
            }
            if (is_black(sib->left) && is_black(sib->right)) {
                sib->color = RED;
                x = parent;
                parent = x->parent;
            } else {
                if (is_black(sib->left)) {
                    if (sib->right != NULL) sib->right->color = BLACK;
                    sib->color = RED;
                    rotate_left(t, sib);
                    sib = parent->left;
                }
                sib->color = parent->color;
                parent->color = BLACK;
                if (sib->left != NULL) sib->left->color = BLACK;
                rotate_right(t, parent);
                return;
            }
        }
    }
    if (x != NULL) x->color = BLACK;
}
```

Style note: mirrors `insert_fixup`'s shape exactly (one function, if/else mirror branch, no
separate mirrored function) -- same precedent already established in this file.

## Integration point in `rb_delete`

Replace the current deferred comment (lines ~207-211):
```c
    if (n_was_black && child != NULL) {
        child->color = BLACK;
    }
    /* else if (n_was_black): doubly-black black-leaf case -- STEP2 fixup
     * loop deferred to a later slice. */
```
with:
```c
    if (n_was_black) {
        if (child != NULL) {
            child->color = BLACK;                 /* STEP1: absorbs the debt locally */
        } else {
            delete_fixup(t, n->parent, NULL);      /* STEP2: only after transplant has run */
        }
    }
```
Nothing else in `rb_delete` changes. `n->parent` is safe to pass here specifically because
`transplant(t, n, child)` (already called just above) has by this point already overwritten
`n->parent`'s child-slot pointer (`->left` or `->right`) to hold `child` (i.e. `NULL`) --
so `delete_fixup`'s very first comparison, `x == parent->left`, correctly resolves which side
is deficient even though `x` is `NULL`, with no extra bookkeeping.

## Independent review (Plan sub-agent, fresh read of the actual code/NOTES.md)

Checked and confirmed:
- **Mirror correctness**: every left/right and `rotate_left`/`rotate_right` pairing verified
  swapped consistently; the one detail people usually get backwards (which nephew is "far") is
  correct in both branches.
- **`sib` is never NULL** when dereferenced: proven from the black-height invariant (a doubly-
  black `x`'s side owes 2+ black-height, so by invariant 3 `sib`'s subtree must also carry
  black-height >=2, which `NULL` (bh 1) can't satisfy) -- not just asserted.
- **Termination**: Case 5 falls through into the same iteration (no bug); Case 4 always
  `return`s; Case 3 is the only path that re-loops, and it strictly climbs one level each time,
  bounded by the root.
- **No use-after-free**: `delete_fixup` runs before `n` itself is freed in `rb_delete`, and
  never touches `n`.
- Hand-traced both real table fixtures end-to-end:
  - `c,b,d,f,g,l,n` / delete `b`: Case 5 (sibling `f` red) -> rotate+swap -> Case 3 (new
    sibling `d`, both children black) -> recolor `d` red, climb to old-parent `c`, `c` was
    reddened by the Case-5 swap so the loop exits immediately -> `c` forced black. Final tree
    balanced, matches the expected Case5->Case3a chain from NOTES.md.
  - `p,n,l,k,c,i,e` / delete `p`: exact mirror image of the above (`k`<->`f`, `e`<->`l`,
    `n`<->`c`, `i`<->`d`), same Case5(mirror)->Case3(mirror) chain, same clean termination.
    (One transcription slip on my part fed the reviewing sub-agent a garbled version of this
    fixture that looked invariant-violating -- re-checked directly against the actual
    committed test sequence and the tree is valid; the design and the real fixture are both
    fine.)

No correctness bugs found. One documentation-only nit taken: the integration comment now says
the call must happen *after* `transplant` has run (the load-bearing fact), not that `n->parent`
must be *read* after `transplant` (which is true but not why it matters -- `transplant` never
writes `n->parent` itself, only `n->parent`'s child-slot).

## Verification plan (post-approval)

1. `make test` -- expect all 9 table-driven `rb_delete` cases green, plus the rest of the
   existing suite unaffected.
2. `make asan`, `make memcheck` -- clean, including the empty-tree case.
3. Out of scope for *this* slice, flagged as an immediate follow-up: `tests/fuzz.c` doesn't
   exercise `rb_delete` yet (per CLAUDE.md project status, "since that function doesn't exist
   yet") -- now that it will, extending the fuzzer to interleave delete ops against the
   reference model is the natural next task, but it's a separate change to a different file
   and gets its own approval pass rather than riding in on this one.

## Open items before implementation begins

1. Approve the `delete_fixup` design and integration point above.
2. Confirm the fuzzer extension is deliberately out of scope for this slice (separate
   follow-up), not something to fold in silently.
