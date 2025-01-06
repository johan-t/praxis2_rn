#include <data.h>

#include <stdlib.h>
#include <string.h>

static struct tuple *find_empty_slot(struct tuple *tuples, size_t n_tuples) {
    for (size_t i = 0; i < n_tuples; i += 1) {
        if (tuples[i].key == NULL) {
            return &tuples[i];
        }
    }
    return NULL;
}

struct tuple *find(string key, struct tuple *tuples, size_t n_tuples) {
    for (size_t i = 0; i < n_tuples; i += 1) {
        if (!tuples[i].key) continue;
        if (strcmp(key, tuples[i].key) == 0) {
            return &(tuples[i]);
        }
    }
    return NULL;
}

static void update_tuple_value(struct tuple *tuple, char *value, size_t value_length) {
    free(tuple->value);
    tuple->value = (char *)malloc(value_length * sizeof(char));
    memcpy(tuple->value, value, value_length);
    tuple->value_length = value_length;
}

static void create_new_tuple(struct tuple *tuple, const string key, char *value, size_t value_length) {
    tuple->key = (char *)malloc((strlen(key) + 1) * sizeof(char));
    strcpy(tuple->key, key);
    tuple->value = (char *)malloc(value_length * sizeof(char));
    memcpy(tuple->value, value, value_length);
    tuple->value_length = value_length;
}

const char *get(const string key, struct tuple *tuples, size_t n_tuples, size_t *value_length) {
    struct tuple *tuple = find(key, tuples, n_tuples);
    if (!tuple) return NULL;
    
    *value_length = tuple->value_length;
    return tuple->value;
}

bool set(const string key, char *value, size_t value_length, struct tuple *tuples, size_t n_tuples) {
    struct tuple *tuple = find(key, tuples, n_tuples);
    if (tuple) {
        update_tuple_value(tuple, value, value_length);
        return true;
    }

    tuple = find_empty_slot(tuples, n_tuples);
    if (!tuple) return false;

    create_new_tuple(tuple, key, value, value_length);
    return false;
}

static void clear_tuple(struct tuple *tuple) {
    free(tuple->key);
    free(tuple->value);
    tuple->key = NULL;
    tuple->value = NULL;
    tuple->value_length = 0;
}

bool remove_tuple(const string key, struct tuple *tuples, size_t n_tuples) {
    struct tuple *tuple = find(key, tuples, n_tuples);
    if (!tuple) return false;

    clear_tuple(tuple);
    return true;
}
