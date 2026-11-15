#define _POSIX_C_SOURCE 200809L

#include "bptree.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

#define MAX_RECORDS 1000000u
#define QUERY_COUNT 2000u

static uint32_t random_state = 0x4d696e69u;
static volatile uint64_t result_sink;

static double now_seconds(void)
{
#ifdef _WIN32
    LARGE_INTEGER ticks;
    LARGE_INTEGER frequency;
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart == 0
        || !QueryPerformanceCounter(&ticks)) return 0.0;
    return (double)ticks.QuadPart / (double)frequency.QuadPart;
#else
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time) != 0) return 0.0;
    return (double)time.tv_sec + (double)time.tv_nsec / 1000000000.0;
#endif
}

static uint32_t next_random(void)
{
    random_state = random_state * 1664525u + 1013904223u;
    return random_state;
}

static int run_comparison(const BPlusTree *tree, char (*keys)[MAX_TAG_LENGTH],
    uint32_t count)
{
    double start;
    double indexed_seconds;
    double linear_seconds;
    uint64_t checksum = 0;
    uint32_t i;

    start = now_seconds();
    for (i = 0; i < QUERY_COUNT; ++i) {
        uint32_t number = next_random() % count + 1;
        char key[MAX_TAG_LENGTH];
        RecordRef ref;
        (void)snprintf(key, sizeof(key), "%07" PRIu32, number);
        if (bptree_search(tree, key, &ref) != DB_OK
            || ref.slot != number - 1) return 0;
        checksum += ref.slot;
    }
    indexed_seconds = now_seconds() - start;

    random_state = 0x4d696e69u;
    start = now_seconds();
    for (i = 0; i < QUERY_COUNT; ++i) {
        uint32_t number = next_random() % count + 1;
        uint32_t j;
        char key[MAX_TAG_LENGTH];
        (void)snprintf(key, sizeof(key), "%07" PRIu32, number);
        for (j = 0; j < count && strcmp(keys[j], key) != 0; ++j) {
        }
        if (j == count) return 0;
        checksum += j;
    }
    linear_seconds = now_seconds() - start;
    result_sink = checksum;
    printf("%" PRIu32 " records, %u lookups: index %.6f s, linear %.6f s\n",
        count, QUERY_COUNT, indexed_seconds, linear_seconds);
    return 1;
}

int main(void)
{
    static const uint32_t checkpoints[] = {10000u, 100000u, MAX_RECORDS};
    char (*keys)[MAX_TAG_LENGTH] =
        (char (*)[MAX_TAG_LENGTH])malloc((size_t)MAX_RECORDS * sizeof(*keys));
    BPlusTree *tree = bptree_create();
    uint32_t key;
    size_t checkpoint = 0;

    if (keys == NULL || tree == NULL) {
        fprintf(stderr, "benchmark setup failed: out of memory\n");
        free(keys);
        bptree_destroy(tree);
        return 1;
    }
    for (key = 1; key <= MAX_RECORDS; ++key) {
        RecordRef ref = {key - 1};
        (void)snprintf(keys[key - 1], MAX_TAG_LENGTH, "%07" PRIu32, key);
        if (bptree_insert(tree, keys[key - 1], ref) != DB_OK) {
            fprintf(stderr, "benchmark insertion failed at key %" PRIu32 "\n", key);
            free(keys);
            bptree_destroy(tree);
            return 1;
        }
        if (checkpoint < sizeof(checkpoints) / sizeof(checkpoints[0])
            && key == checkpoints[checkpoint]) {
            if (!run_comparison(tree, keys, key)) {
                fprintf(stderr, "benchmark lookup check failed at %" PRIu32 "\n", key);
                free(keys);
                bptree_destroy(tree);
                return 1;
            }
            ++checkpoint;
        }
    }
    if (bptree_check(tree) != DB_OK) {
        fprintf(stderr, "benchmark tree invariant check failed\n");
        free(keys);
        bptree_destroy(tree);
        return 1;
    }
    free(keys);
    bptree_destroy(tree);
    return 0;
}
