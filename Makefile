CC := gcc
CFLAGS := -std=c23 -Wall -Wextra -Werror -g -O1 -Iinclude -Itests
# TEST_CFLAGS is recursively expanded (=, not :=) so it picks up the
# asan target's target-specific `CFLAGS +=` override at build time.
TEST_CFLAGS = $(CFLAGS) -DRBTREE_TEST_HOOKS
SRC := src/rbtree.c src/pool.c
TSRC := tests/test_rbtree.c
FAULT_SRC := tests/fault_alloc.c
FAULT_HDR := tests/fault_alloc.h
BIN := build/test_rbtree
FUZZBIN := build/fuzz

all: $(BIN) $(FUZZBIN)

$(BIN): $(SRC) $(TSRC) $(FAULT_SRC) $(FAULT_HDR) include/rbtree.h
	@mkdir -p build
	$(CC) $(TEST_CFLAGS) $(SRC) $(TSRC) $(FAULT_SRC) -o $@

$(FUZZBIN): $(SRC) tests/fuzz.c $(FAULT_SRC) $(FAULT_HDR) include/rbtree.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SRC) tests/fuzz.c $(FAULT_SRC) -o $@

# The fuzzer runs the full op count in BOTH node-storage modes: a pooled tree
# that has not been fuzzed as hard as the heap one is not covered. "-" keeps
# the default time(NULL) seeding in both passes.
test: $(BIN) $(FUZZBIN)
	./$(BIN) && ./$(FUZZBIN) 100000 - heap && ./$(FUZZBIN) 100000 - pooled

# Both gates rebuild from scratch, and the order must be GUARANTEED rather
# than incidental. So `clean` is the sole prerequisite of each and the build
# is driven by a recursive $(MAKE) in the recipe.
#
# Why not `asan: clean test` / `memcheck: clean all`: those name siblings,
# and -j is free to run them concurrently -- `clean` can delete build/ after
# the other populated it, or race it mid-compile. Even serially, make decides
# whether `test`/`all` is up to date from the binaries present when it builds
# its dependency graph, which immediately after a `make asan` are the
# ASan-instrumented ones.
#
# For memcheck that produced a gate which passed having tested nothing:
# valgrind over an ASan-built binary reports "0 allocs, 0 frees" and "All
# heap blocks were freed -- no leaks are possible", so --error-exitcode=1
# never fires and the run reads as success. One prerequisite plus an ordered
# recipe makes clean-then-build the only possible sequence at any -j.
#
# asan hands its effective CFLAGS to the sub-make explicitly. The
# target-specific `+=` below IS in scope inside asan's own recipe, so
# $(CFLAGS) there is already the augmented value; passing it as a command-line
# assignment is what carries it across the recursive invocation, where the
# `asan` target is never visited and its `+=` would therefore never apply.
# TEST_CFLAGS stays recursively expanded (=, not :=) so the hooks build picks
# the sanitizer flags up too.
asan: CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer
asan: clean
	$(MAKE) test CFLAGS="$(CFLAGS)"

memcheck: clean
	$(MAKE) all
	valgrind --leak-check=full --show-leak-kinds=all \
		--error-exitcode=1 ./$(BIN)
	valgrind --leak-check=full --show-leak-kinds=all \
		--error-exitcode=1 ./$(FUZZBIN) 20000 - heap
	valgrind --leak-check=full --show-leak-kinds=all \
		--error-exitcode=1 ./$(FUZZBIN) 20000 - pooled
clean:
	rm -rf build

.PHONY: all test asan memcheck clean