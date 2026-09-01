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

 