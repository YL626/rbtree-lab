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

asan: CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer
asan: clean test

memcheck: all
	valgrind --leak-check=full --show-leak-kinds=all \
		--error-exitcode=1 ./$(BIN)
	valgrind --leak-check=full --show-leak-kinds=all \
		--error-exitcode=1 ./$(FUZZBIN) 20000 - heap
	valgrind --leak-check=full --show-leak-kinds=all \
		--error-exitcode=1 ./$(FUZZBIN) 20000 - pooled
clean:
	rm -rf build

.PHONY: all test asan memcheck clean