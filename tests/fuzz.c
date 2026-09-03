#include "rbtree.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Reference model: a flat array of key/value pairs, kept in sync with the
 * tree on every successful insert. Linear scans are fine here — this is a
 * correctness oracle, not something that needs to be fast. */
struct model_entry {
    char *key;
    int  *value;
};

struct model {
    struct model_entry *entries;
    size_t               count;
    size_t               capacity;
};

static void model_init(struct model *m) {
    m->entries = NULL;
    m->count = 0;
    m->capacity = 0;
}

static struct model_entry *model_find(struct model *m, const char *key) {
    for (size_t i = 0; i < m->count; i++) {
        if (strcmp(m->entries[i].key, key) == 0) return &m->entries[i];
    }
    return NULL;
}

static void model_put(struct model *m, const char *key, int *value) {
    struct model_entry *e = model_find(m, key);
    if (e != NULL) {
        free(e->value);
        e->value = value;
        return;
    }
    if (m->count == m->capacity) {
        size_t new_cap = m->capacity == 0 ? 64 : m->capacity * 2;
        struct model_entry *grown = realloc(m->entries, new_cap * sizeof *grown);
        if (grown == NULL) { fprintf(stderr, "fuzz: model realloc failed\n"); exit(1); }
        m->entries = grown;
        m->capacity = new_cap;
    }
    m->entries[m->count].key = strdup(key);
    if (m->entries[m->count].key == NULL) { fprintf(stderr, "fuzz: strdup failed\n"); exit(1); }
    m->entries[m->count].value = value;
    m->count++;
}

static void model_destroy(struct model *m) {
    for (size_t i = 0; i < m->count; i++) {
        free(m->entries[i].key);
        free(m->entries[i].value);
    }
    free(m->entries);
}

static void make_key(char *buf, size_t buflen, unsigned key_space) {
    snprintf(buf, buflen, "key%06u", (unsigned)(rand() % key_space));
}

int main(int argc, char **argv) {
    long op_count = argc > 1 ? atol(argv[1]) : 100000;
    unsigned seed = argc > 2 ? (unsigned)atol(argv[2]) : (unsigned)time(NULL);
    unsigned key_space = 5000;
    srand(seed);
    printf("fuzz: seed=%u op_count=%ld\n", seed, op_count);

    rbtree_t *t = rb_create(NULL);
    if (t == NULL) { fprintf(stderr, "fuzz: rb_create failed\n"); return 1; }

    struct model m;
    model_init(&m);

    char keybuf[32];
    for (long op = 0; op < op_count; op++) {
        make_key(keybuf, sizeof keybuf, key_space);
        int choose_insert = rand() % 2;

        if (choose_insert) {
            int *value = malloc(sizeof *value);
            if (value == NULL) { fprintf(stderr, "fuzz: value malloc failed\n"); return 1; }
            *value = (int)op;

            int rc = rb_insert(t, keybuf, value);
            if (rc != 0) {
                fprintf(stderr, "fuzz: rb_insert failed unexpectedly at op %ld\n", op);
                free(value);
                return 1;
            }
            void *found = rb_find(t, keybuf);
            if (found != value) {
                fprintf(stderr, "fuzz: rb_find after insert mismatch at op %ld "
                                 "(key=%s)\n", op, keybuf);
                return 1;
            }
            model_put(&m, keybuf, value);
        } else {
            struct model_entry *e = model_find(&m, keybuf);
            void *found = rb_find(t, keybuf);
            if (e == NULL) {
                if (found != NULL) {
                    fprintf(stderr, "fuzz: rb_find found a key the model doesn't have "
                                     "at op %ld (key=%s)\n", op, keybuf);
                    return 1;
                }
            } else if (found != e->value) {
                fprintf(stderr, "fuzz: rb_find value mismatch at op %ld (key=%s)\n",
                        op, keybuf);
                return 1;
            }
        }

        if (op % 100 == 0) {
            int rc = rb_validate(t);
            if (rc != 0) {
                fprintf(stderr, "fuzz: rb_validate failed at op %ld (invariant %d)\n",
                        op, rc);
                return 1;
            }
        }

        if (rb_size(t) != m.count) {
            fprintf(stderr, "fuzz: rb_size (%zu) != model count (%zu) at op %ld\n",
                    rb_size(t), m.count, op);
            return 1;
        }
    }

    if (rb_validate(t) != 0) {
        fprintf(stderr, "fuzz: final rb_validate failed\n");
        return 1;
    }

    rb_destroy(t);
    model_destroy(&m);

    printf("fuzz: %ld insert/find ops OK\n", op_count);
    return 0;
}
