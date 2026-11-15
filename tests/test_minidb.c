#include "bptree.h"
#include "parser.h"
#include "query.h"
#include "record.h"
#include "storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <process.h>
#define test_pid() _getpid()
#else
#include <unistd.h>
#define test_pid() getpid()
#endif

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static unsigned int random_state = 0x4d696e69u;

static unsigned int next_random(void)
{
    random_state = random_state * 1664525u + 1013904223u;
    return random_state;
}

static void shuffle(unsigned int *values, unsigned int count)
{
    unsigned int i;
    for (i = count; i > 1; --i) {
        unsigned int j = next_random() % i;
        unsigned int temporary = values[i - 1];
        values[i - 1] = values[j];
        values[j] = temporary;
    }
}

static int test_tree(void)
{
    enum { KEY_COUNT = 1200 };
    unsigned int values[KEY_COUNT];
    unsigned int i;
    BPlusTree *tree = bptree_create();
    CHECK(tree != NULL);

    for (i = 0; i < KEY_COUNT; ++i) values[i] = i + 1;
    shuffle(values, KEY_COUNT);
    for (i = 0; i < KEY_COUNT; ++i) {
        char key[MAX_TAG_LENGTH];
        RecordRef ref = {values[i] * 3u};
        (void)snprintf(key, sizeof(key), "%04u", values[i]);
        CHECK(bptree_insert(tree, key, ref) == DB_OK);
        CHECK(bptree_check(tree) == DB_OK);
    }
    for (i = 1; i <= KEY_COUNT; ++i) {
        char key[MAX_TAG_LENGTH];
        RecordRef ref;
        (void)snprintf(key, sizeof(key), "%04u", i);
        CHECK(bptree_search(tree, key, &ref) == DB_OK);
        CHECK(ref.slot == i * 3u);
    }
    {
        RecordRef ref = {0};
        CHECK(bptree_insert(tree, "0007", ref) == DB_ERR_DUPLICATE);
    }
    shuffle(values, KEY_COUNT);
    for (i = 0; i < KEY_COUNT; ++i) {
        char key[MAX_TAG_LENGTH];
        RecordRef ref;
        (void)snprintf(key, sizeof(key), "%04u", values[i]);
        CHECK(bptree_delete(tree, key) == DB_OK);
        CHECK(bptree_search(tree, key, &ref) == DB_ERR_NOT_FOUND);
        CHECK(bptree_check(tree) == DB_OK);
    }
    CHECK(bptree_check(tree) == DB_OK);
    bptree_destroy(tree);

    tree = bptree_create();
    CHECK(tree != NULL);
    {
        static const char *keys[] = {"b", "aa", "a", "10", "2"};
        static const char *expected[] = {"10", "2", "a", "aa", "b"};
        BPIter iterator;
        char actual[MAX_TAG_LENGTH];
        RecordRef ref;
        for (i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
            RecordRef item_ref = {i};
            CHECK(bptree_insert(tree, keys[i], item_ref) == DB_OK);
        }
        CHECK(bptree_iter_first(tree, &iterator) == DB_OK);
        for (i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
            CHECK(bptree_iter_next(&iterator, actual, &ref));
            CHECK(strcmp(actual, expected[i]) == 0);
        }
        CHECK(!bptree_iter_next(&iterator, actual, &ref));
        CHECK(bptree_check(tree) == DB_OK);
    }
    bptree_destroy(tree);
    return 0;
}

static int test_parser_and_record(void)
{
    Command command;
    Record input;
    Record output;
    uint8_t bytes[RECORD_DISK_SIZE];

    CHECK(command_parse("INSERT 001 \"Ava Student\" 9.25", &command) == DB_OK);
    CHECK(command.type == CMD_INSERT && command.value_count == 3);
    CHECK(strcmp(command.values[0], "001") == 0);
    CHECK(strcmp(command.values[1], "Ava Student") == 0);
    CHECK(strcmp(command.values[2], "9.25") == 0);
    CHECK(command_parse("SELECT 17 trailing", &command) == DB_ERR_SYNTAX);
    CHECK(command_parse("INSERT \"\" Name 9.25", &command) == DB_OK);
    memset(&input, 0, sizeof(input));
    strcpy(input.values[0], "001");
    strcpy(input.values[1], "Ava Student");
    strcpy(input.values[2], "9.25");
    CHECK(record_validate(&input, 3) == DB_OK);
    CHECK(record_serialize(&input, bytes) == DB_OK);
    CHECK(record_deserialize(bytes, 3, &output) == DB_OK);
    CHECK(strcmp(output.values[0], input.values[0]) == 0);
    CHECK(strcmp(output.values[1], input.values[1]) == 0);
    bytes[1] = 1;
    CHECK(record_deserialize(bytes, 3, &output) == DB_ERR_CORRUPT);
    return 0;
}

static int execute_ok(Db *db, const char *line, FILE *output)
{
    int exit_requested = 0;
    return db_execute(db, line, output, &exit_requested) == DB_OK;
}

static int choose_test_paths(char *path, size_t path_size,
    char *lock_path, size_t lock_size)
{
    unsigned int suffix;
    for (suffix = 0; suffix < 10000; ++suffix) {
        FILE *file;
        if (snprintf(path, path_size, "minidb-test-%lu-%u.db",
                (unsigned long)test_pid(), suffix) >= (int)path_size
            || snprintf(lock_path, lock_size, "%s.lock", path) >= (int)lock_size) {
            return 0;
        }
        file = fopen(path, "rb");
        if (file != NULL) {
            fclose(file);
            continue;
        }
        file = fopen(lock_path, "rb");
        if (file != NULL) {
            fclose(file);
            continue;
        }
        return 1;
    }
    return 0;
}

static int test_database(void)
{
    char path[128];
    char lock_path[136];
    Db *db = NULL;
    Db *second = NULL;
    FILE *output;
    int was_dirty = 0;
    int exit_requested = 0;
    DbStatus status;

    CHECK(choose_test_paths(path, sizeof(path), lock_path, sizeof(lock_path)));
    output = tmpfile();
    CHECK(output != NULL);
    CHECK(db_open(path, &db, &was_dirty) == DB_OK);
    CHECK(was_dirty == 0 && db_live_count(db) == 0);
    CHECK(db_open(path, &second, NULL) == DB_ERR_LOCKED);
    CHECK(second == NULL);
    CHECK(storage_column_count(NULL) == 0);
    CHECK(execute_ok(db, "RENAME COLUMN tag RNO", output));
    CHECK(execute_ok(db, "RENAME COLUMN data Name", output));
    CHECK(execute_ok(db, "ADD COLUMN CGPA", output));
    CHECK(execute_ok(db, "INSERT 003 \"Ava Student\" 9.25", output));
    CHECK(execute_ok(db, "INSERT 010 \"Bo Student\" 8.50", output));
    CHECK(db_live_count(db) == 2);
    CHECK(db_execute(db, "INSERT 003 Duplicate 4.00", output, &exit_requested)
        == DB_ERR_DUPLICATE);
    CHECK(execute_ok(db, "ADD COLUMN Course", output));
    CHECK(execute_ok(db, "UPDATE 003 Course \"Math Calculus\"", output));
    CHECK(execute_ok(db, "SELECT RANGE 001 020", output));
    CHECK(execute_ok(db, "DISPLAY LIMIT 1", output));
    CHECK(db_close(db) == DB_OK);
    db = NULL;

    CHECK(db_open(path, &db, &was_dirty) == DB_OK);
    CHECK(was_dirty == 0 && db_live_count(db) == 2);
    CHECK(execute_ok(db, "SELECT 003", output));
    CHECK(db_execute(db, "SELECT 020", output, &exit_requested) == DB_ERR_NOT_FOUND);
    CHECK(execute_ok(db, "DELETE 010", output));
    CHECK(db_live_count(db) == 1);
    CHECK(db_close(db) == DB_OK);
    db = NULL;

    {
        FILE *file = fopen(path, "r+b");
        unsigned char dirty = 1;
        CHECK(file != NULL);
        CHECK(fseek(file, 28, SEEK_SET) == 0);
        CHECK(fwrite(&dirty, 1, 1, file) == 1);
        CHECK(fclose(file) == 0);
    }
    CHECK(db_open(path, &db, &was_dirty) == DB_OK);
    CHECK(was_dirty == 1 && db_live_count(db) == 1);
    CHECK(db_close(db) == DB_OK);
    db = NULL;

    {
        FILE *file = fopen(path, "r+b");
        int bad = 'X';
        CHECK(file != NULL);
        CHECK(fseek(file, 0, SEEK_SET) == 0);
        CHECK(fwrite(&bad, 1, 1, file) == 1);
        CHECK(fclose(file) == 0);
    }
    status = db_open(path, &db, NULL);
    CHECK(status == DB_ERR_CORRUPT && db == NULL);
    CHECK(fclose(output) == 0);
    CHECK(remove(path) == 0);
    CHECK(remove(lock_path) != 0);
    return 0;
}

int main(void)
{
    if (test_parser_and_record() != 0) return 1;
    if (test_tree() != 0) return 1;
    if (test_database() != 0) return 1;
    puts("All MiniDB tests passed.");
    return 0;
}
