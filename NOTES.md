**Contracts**: 

include/rb_tree.h (the frozen contract)
rbtree.c implements rb_tree.h
src/rbtree.c (tree logic, fixups, validate, teardown)
rb_malloc / rb_free (the optional seam)
test_rbtree.c + fuzz.c call through rb_tree.h
tests/test_rbtree.c (the unit tests + the deletion table)
tests/fuzz.c (driver + reference model)

At every moment, every allocation has exactly one owner, and that owner is responsible for freeing it.

rb_insert is the instructive case.
On success, the tree takes ownership of value; the caller must not free it, ever, because the tree
will, through the value_free function supplied at creation. On failure, ownership never moved:
the caller still holds value and remains responsible for it. And on an overwrite of an existing key,
the tree frees the old value before installing the new one, because at that moment the tree is the old
value’s only owner and nobody else will ever see it again

Implement a classic red-black tree behind the fixed API in include/rbtree.h (see Appendix A).
Keys are C strings; the tree copies keys on insert and owns the copies. Values are opaque void *
pointers; the tree takes ownership of a value on successful insert and releases it (via the destructor
supplied to rb_create) on delete/destroy/overwrite. 

Every node is red or black, the root is black. 
Conceptually, every missing child ends at a black sentinel leaf, written NIL. A red node only has black children, so reds never stack. 
Every path from a given node down to any NIL leaf crosses the same number of black nodes.This count is called the black-height. des. The long path can
therefore be at most twice the length of the short one. From this it follows that the height satisfies h ≤ 2 log2(n + 1), and search, insert, and delete all run in O(log n) worst-case time.

 In-order traversal yields the keys in sorted order. 
Insert fixup needs at most two rotations; delete fixup needs at most three.
do not make the validator responsible for keeping t->root correct. The rotation owns that job

And do save the child pointer before you free the
parent, not after, because for one instruction that pointer is the only thing standing between you
and a subtree nobody can reach anymore.

When you are done with a pointer (*p) you must free(p) it. Only once, then never touch anything that pointer touched again. 

The header is frozen. rb_insert returns −1 and
the caller still owns the value.
Overwriting frees the old value
through value_free, and
nobody else does.

Required operations: rb_create, rb_insert (with rebalancing), rb_find, rb_delete
(with the full deletion-fixup cases; yes, all of them), rb_size, rb_foreach (in-order), rb_validate,
rb_destroy.

rb_validate must check, and your tests must exercise, all invariants (Section 3, Figure 1):
1. The root is black.
2. No red node has a red child.
3. Every root-to-NIL path contains the same number b of black nodes (the tree’s black-height); so
the tree height is O(log n).
4. In-order traversal yields strictly increasing keys: k1 < k2 < · · · < kn under strcmp.
5. rb_size matches the actual node count n

you must also write table-driven tests for rb_delete covering, at minimum: a red leaf, a
black leaf with a red sibling, a node with two children, and deletion of the root. Each test asserts
rb_validate and rb_size afterward.

Memory rules: rb_destroy must free every node together with its key copy and, when a
destructor was supplied, every value as well; both verification builds must come back with zero
findings across your entire suite, empty tree included, because you tested the empty tree. Recursion
is permitted this time, in rb_destroy and rb_foreach alike. Savor that. The next assignment
revokes it, and Section 10 lets you find out early what the revocation costs.

Route all allocation in src/rbtree.c through two four line wrappers. rb_malloc and rb_free, that simply forward to malloc and free. 

you will be required to tear down a
million-node tree in O(1) auxiliary space: no recursion, no heap-allocated stack, a couple of local
variables and nothing more.
The classic trick is lovely. As you free, rotate the tree into a right spine, so there is never a left
subtree left to remember; the tree’s own pointer fields become the bookkeeping. If you want a head
start on next month, implement it now behind a second function and check it against your fuzzer’s
teardown. 

plan first, keep the diffs small, verify everything with tools, and merge
nothing the user cannot explain.

rb_validate must show a failure reason when it breaks a rule.

 fixup must never dereference memory that has already been freed.

account for the deletion case of deleting a black node with exactly one child. 

**Thresholds**: 
your fuzzer (tests/fuzz.c) performs ≥ 10^5
random insert/find/delete operations against a reference model (a sorted array or a simple linked list is fine), calling rb_validate
at least every 100 operations, under both asan and memcheck.

**Choices left to me**: 

We will add the optional seam, rb_malloc and rb_free (routing as seen in contracts)

I would like to add a parent pointer. 

The mirror image (a right child of a right child, needing a left rotation at the grandparent) is
the same code with left and right swapped, and it is the half people forget. Do not forget it.
And it is worth handing this exact sequence to Claude Code in plan mode, asking it to predict each
panel before you scroll, because arguing about 35 for ten minutes now is considerably cheaper than
discovering at the walkthrough that you learned deletion fixup and merely memorized this.
FIGURE 4





**Confusions:**

**Personal Notes**
We did not use a shared sentinel due to how every parent with a missing child would claim that that one shared sentinel is their child, when in reality the parent which most recently wrote to it, is the one who is the parent.

We did not decide to update parent pointers during destruction/teardown due to the fact that it will all be destroyed regardless, so there is not benefit to the additional writes. 

rb_create can return null when allocation fails. There is no clean up because the only allocation was the failed attempt so there is nothing else to unwind. 

You only ever act during insert-fixup if there is a red-red violation
 1. if the uncle is red, recolor and run up the tree to the root.
 2. if the uncle is black or NIL, rotate. 
 2a. If x and parent are left left or right right, rotate at the root.
 2b. if x and parent are right left or left right, rotate the parent to straighten a line, then at the grandparent to finsih it. 

Invariant 1: Root is red
Invariant 2: red-red violation
Invariant 3: black height violation




The loop invariant (insert_fixup, src/rbtree.c:74-137), at the top of every iteration — i.e., exactly when while (is_red(z->parent)) is checked:

1. z is red.
2. If z's parent is the root, that parent is black. (Equivalently: whenever the loop body actually runs, z->parent is not the root.)
3. Every red-black property that could be violated (no-red-red, equal black-height) has at most one violation in the whole tree, and it is exactly the edge z–z->parent both being red. Everywhere else — including every black-height count — the tree is a legal red-black tree.

Part 2 is the load-bearing one for the code as written: grandparent = parent->parent (line 79) is dereferenced unconditionally with no NULL check. That's only safe because the loop condition already guarantees parent isn't the root — if it were, invariant 2 says it'd be black, contradicting is_red(z->parent) being true. So the invariant isn't just descriptive, it's what makes that line not a null-pointer bug.

Why the invariant holds initially: z is a freshly linked node, colored RED in rb_insert right before insert_fixup(t, n) is called (line 154) — that's (1). (2) and (3) hold because every previous call to rb_insert ended with t->root->color = BLACK (line 136) and left the rest of the tree fully valid — so before this insertion touched anything, the tree was a legitimate red-black tree, and linking in one red leaf can only create the single red-red violation at that leaf's edge to its parent (if any).

Why each branch preserves it:
- Red-uncle case (lines 81-85 / mirrored 100-104): recolor parent, uncle black and grandparent red, then z = grandparent. Black-height is unchanged on every root-to-leaf path through this subtree (one black node became two black nodes on divergent paths, one black node became red, net black-count per path is identical — the standard CLRS argument). The only possible new violation is grandparent (now red) clashing with its parent — which is exactly invariant 3, now anchored at the new z.
- Black-uncle triangle case (lines 87-90): a rotation at z's old position that only reshapes the local subtree without changing anyone's color or black-height; it converts a "bent" violation into a "straight" one so the next case applies.
- Black-uncle line case (lines 91-93): recolors parent black, grandparent red, rotates at grandparent. This is the terminal fixer — the red-red violation is resolved and no new one is introduced, because the node that ends up on top (parent) is black.

Termination. Define depth(z) as z's distance from the root. Every iteration does exactly one of two things:

- Red-uncle case: reassigns z to grandparent, so depth(z) strictly decreases by 2. The loop condition is re-tested on a strictly shallower node.
- Black-uncle case (either sub-case): after the rotation, z's parent is the node just c2/111). So is_red(z->parent) is false the instant the loop re-checks — this is provablythe last iteration, not just usually.

So the sequence of iterations is: zero or more red-uncle steps, each strictly decreasing a non-negative integer (depth(z), bounded below by 0, since depth(root) == 0 and is_red(NULL) is false so the loop stops there too), followed by at most one black-uncle step that unconditionally ends th sequence of non-negative integers is finite, so the climb terminates in at mostdepth(z_initial) iterations — bounded by the tree height, i.e. O(log n) after this milestone's fixup is in place.

I can defend this at the walkthrough. Nothing here contradicts the six test cases already passing — the red-uncle test (test_insert_triggers_red_uncle_recolor) exercises exactly one continuing iteration before the final t->root->color = BLACK catches the loop-exit-with-red-root edge case, se a black-uncle case landing directly on the terminal branch.



Self-tests for rb_find and rb_insert (confirm you can answer these before moving on — they're exactly the kind of
 question the live walkthrough asks):

 1. cmp is declared once, before the loop, and reused after the loop ends to decide
    parent->left vs parent->right. The loop's own condition (cur != NULL) has already
    gone false by the time you read cmp again — why is cmp's value from the last
    iteration still exactly the comparison you need, and what would go wrong if you
    re-declared cmp fresh (uninitialized) after the loop instead of reusing it?
 2. When parent == NULL (the tree was empty), cmp is never assigned — it keeps its
    initializer value of 0. Why is that safe, given the if (parent == NULL) branch is
    checked first in the three-way if/else if/else?
 3. The header says value_free "may be NULL (values not owned)." In the overwrite branch,
    if value_free is NULL, the old value is never freed and its pointer is simply
    overwritten and lost. Is that a leak the tree is responsible for? Re-read the ownership
    contract in rbtree.h and NOTES.md before answering — the answer determines whether
    this is a bug or exactly the documented contract.
 4. rb_find takes const rbtree_t *t but walks struct rb_node *cur (non-const) initialized
    from t->root. Does returning cur->value (a void *) from a function that only received
    a const rbtree_t * leak a way for the caller to mutate the tree's internal nodes? Compare
    this to what "the tree retains ownership" (the header's comment on rb_find) is actually
    promising versus what const can enforce.

 Self-tests before moving on (counting_free, make_int, test_overwrite_frees_old_value, test_overwrite_with_null_value_free_does_not_crash):

 1. free_count is reset to 0 at the top of test_overwrite_frees_old_value, even though
    it's a file-scope global initialized to 0 already. Why is that reset necessary given
    tests run one after another inside a single main(), in the same process?
 2. The test asserts free_count == 2 after rb_destroy(t), not free_count == 1. Walk
    through exactly which value each of the two counting_free calls fires on, and in which
    function (rb_insert's overwrite branch vs. free_subtree's post-order walk) each call
    originates.
 3. test_overwrite_with_null_value_free_does_not_crash calls no destructor at all and checks
    no counter. What specific claim is it proving that test_overwrite_frees_old_value does
    not already cover? (Hint: re-read increment 2's self-test #3 about what value_free == NULL
    means for who's responsible for the old value.)



 Self-tests before moving on(for rb_validate, validate_walk, rb_foreach, foreach_inorder):

 1. validate_walk compares each node's key only to st->prev_key — the key of whichever
    node was visited immediately before it in the in-order walk, which may be nowhere near
    it in the tree's actual shape (not its parent, not its sibling). Why is "compare to the
    previous in-order-visited node" the correct and complete way to check "strictly increasing
    under strcmp," rather than something that needs to also compare to a node's parent or
    children directly?
 2. The check is strcmp(...) >= 0, not > 0. What specific tree state would a >= 0 check
    catch that a > 0 check would miss, and can that state actually occur given how
    rb_insert's overwrite branch works? (This connects back to increment 2's ownership
    walkthrough — think about whether two nodes with equal keys could ever coexist.)
 3. rb_validate currently can only ever return 0, 4, or 5 — never 1, 2, or 3,
    because those checks don't exist yet. If you called rb_validate right now on a tree that
    would fail invariant 2 once fixup exists (e.g. hypothetically two adjacent red nodes),
    what would it actually return today, and why is that the honest, correct answer for this
    increment rather than a bug?

Self-tests before moving on(for rb_destroy):

 1. Convince yourself this never reads cur after freeing it — next is captured before
    any rb_free call on the free-leaf branch, and the rotation branch never frees anything
    at all, it only rewires. This is exactly the "save the child pointer before you free the
    parent, not after" warning from NOTES.md, applied to a loop instead of a single splice.
 2. Trace a concrete 3-node left-leaning chain by hand: root "c" with left child "b" with
    left child "a" (built by inserting "c", "b", "a" in that order). Walk the loop
    iteration by iteration — which iterations rotate, which iterations free a node, and in
    what order do "a", "b", "c" actually get freed? (They are not freed in tree order,
    nor strictly leaf-to-root — work out the actual sequence.)
 3. Why does the rotation branch (cur->left != NULL) never need to touch cur->right at
    all, even though cur is about to become left's right child? (Compare against a normal
    left-rotate from Figure 2 of the spec — this teardown rotation is doing less work than a
    real tree rotation, on purpose. What invariant does a real rotation preserve that this one
    is explicitly allowed to ignore, and why, given the earlier "leave parent pointers stale
    during teardown" decision?)