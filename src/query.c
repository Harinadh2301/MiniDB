#include "query.h"

#include "bptree.h"
#include "parser.h"
#include "record.h"
#include "storage.h"

#include <stdlib.h>
#include <string.h>

struct Db {
    Storage *storage;
    BPlusTree *tree;
};

static DbStatus verify_reference(Db *db, const char *tag, RecordRef ref,
    Record *record)
{
    DbStatus status = storage_read_record(db->storage, ref, record);
    if (status != DB_OK) return status;
    if (record->deleted || strcmp(record->values[0], tag) != 0) {
        return DB_ERR_INTERNAL;
    }
    return DB_OK;
}

DbStatus db_open(const char *path, Db **out, int *was_dirty)
{
    Db *db;
    uint32_t slot;
    uint32_t live_count = 0;
    DbStatus status;

    if (out == NULL) return DB_ERR_INVALID;
    *out = NULL;
    db = (Db *)calloc(1, sizeof(*db));
    if (db == NULL) return DB_ERR_NOMEM;
    status = storage_open(path, &db->storage, was_dirty);
    if (status != DB_OK) {
        free(db);
        return status;
    }
    db->tree = bptree_create();
    if (db->tree == NULL) {
        storage_abort(db->storage);
        free(db);
        return DB_ERR_NOMEM;
    }

    for (slot = 0; slot < storage_slot_count(db->storage); ++slot) {
        Record record;
        RecordRef ref = {slot};
        status = storage_read_record(db->storage, ref, &record);
        if (status != DB_OK) {
            db_abort(db);
            return status == DB_ERR_IO ? status : DB_ERR_CORRUPT;
        }
        if (record.deleted) continue;
        status = bptree_insert(db->tree, record.values[0], ref);
        if (status != DB_OK) {
            db_abort(db);
            return status == DB_ERR_NOMEM ? status : DB_ERR_CORRUPT;
        }
        if (live_count == UINT32_MAX) {
            db_abort(db);
            return DB_ERR_CORRUPT;
        }
        ++live_count;
    }
    status = bptree_check(db->tree);
    if (status == DB_OK) {
        status = storage_reconcile_live_count(db->storage, live_count);
    }
    if (status != DB_OK) {
        db_abort(db);
        return status;
    }
    *out = db;
    return DB_OK;
}

static DbStatus check_tree_after_mutation(const BPlusTree *tree)
{
#ifndef NDEBUG
    return bptree_check(tree);
#else
    (void)tree;
    return DB_OK;
#endif
}

static DbStatus print_border(FILE *out, const size_t *widths, uint32_t count)
{
    uint32_t column;
    for (column = 0; column < count; ++column) {
        size_t i;
        if (fputc('+', out) == EOF) return DB_ERR_IO;
        for (i = 0; i < widths[column] + 2; ++i) {
            if (fputc('-', out) == EOF) return DB_ERR_IO;
        }
    }
    return fputs("+\n", out) == EOF ? DB_ERR_IO : DB_OK;
}

static DbStatus print_table_header(Db *db, FILE *out, const size_t *widths)
{
    uint32_t column;
    DbStatus status = print_border(out, widths, storage_column_count(db->storage));
    if (status != DB_OK) return status;
    for (column = 0; column < storage_column_count(db->storage); ++column) {
        if (fprintf(out, "| %-*s ", (int)widths[column],
                storage_column_name(db->storage, column)) < 0) return DB_ERR_IO;
    }
    if (fputs("|\n", out) == EOF) return DB_ERR_IO;
    return print_border(out, widths, storage_column_count(db->storage));
}

static DbStatus print_record_row(Db *db, FILE *out, const Record *record,
    const size_t *widths)
{
    uint32_t column;
    for (column = 0; column < storage_column_count(db->storage); ++column) {
        if (fprintf(out, "| %-*s ", (int)widths[column],
                record->values[column]) < 0) return DB_ERR_IO;
    }
    return fputs("|\n", out) == EOF ? DB_ERR_IO : DB_OK;
}

static int in_range(const char *key, int range, const char *low, const char *high)
{
    return !range || (strcmp(key, low) >= 0 && strcmp(key, high) <= 0);
}

static DbStatus display_records(Db *db, FILE *out, int range,
    const char *low, const char *high, const char *only_tag,
    int has_limit, uint32_t limit)
{
    BPIter iterator;
    char key[MAX_TAG_LENGTH];
    RecordRef ref;
    uint32_t columns = storage_column_count(db->storage);
    size_t widths[MAX_COLUMNS];
    uint64_t total = 0;
    uint64_t shown;
    uint32_t i;
    DbStatus status;

    if (range && strcmp(low, high) > 0) {
        return fprintf(out, "0 records.\n") < 0 ? DB_ERR_IO : DB_OK;
    }
    for (i = 0; i < columns; ++i) {
        widths[i] = strlen(storage_column_name(db->storage, i));
    }
    if (only_tag != NULL) {
        Record record;
        status = bptree_search(db->tree, only_tag, &ref);
        if (status != DB_OK) return status;
        status = verify_reference(db, only_tag, ref, &record);
        if (status != DB_OK) return status;
        for (i = 0; i < columns; ++i) {
            size_t length = strlen(record.values[i]);
            if (length > widths[i]) widths[i] = length;
        }
        status = print_table_header(db, out, widths);
        if (status != DB_OK) return status;
        status = print_record_row(db, out, &record, widths);
        if (status != DB_OK) return status;
        status = print_border(out, widths, columns);
        if (status != DB_OK) return status;
        return fputs("1 record.\n", out) == EOF ? DB_ERR_IO : DB_OK;
    }

    status = range ? bptree_iter_seek(db->tree, low, &iterator)
                   : bptree_iter_first(db->tree, &iterator);
    if (status != DB_OK) return status;
    while (bptree_iter_next(&iterator, key, &ref)) {
        Record record;
        if (range && strcmp(key, high) > 0) break;
        if (!in_range(key, range, low, high)) continue;
        status = verify_reference(db, key, ref, &record);
        if (status != DB_OK) return status;
        ++total;
        if (!has_limit || total <= limit) {
            for (i = 0; i < columns; ++i) {
                size_t length = strlen(record.values[i]);
                if (length > widths[i]) widths[i] = length;
            }
        }
    }
    shown = has_limit && total > limit ? limit : total;
    if (total == 0) {
        return fprintf(out, "0 records.\n") < 0 ? DB_ERR_IO : DB_OK;
    }
    status = print_table_header(db, out, widths);
    if (status != DB_OK) return status;
    status = range ? bptree_iter_seek(db->tree, low, &iterator)
                   : bptree_iter_first(db->tree, &iterator);
    if (status != DB_OK) return status;
    {
        uint64_t row = 0;
        while (row < shown && bptree_iter_next(&iterator, key, &ref)) {
            Record record;
            if (range && strcmp(key, high) > 0) break;
            if (!in_range(key, range, low, high)) continue;
            status = verify_reference(db, key, ref, &record);
            if (status != DB_OK) return status;
            status = print_record_row(db, out, &record, widths);
            if (status != DB_OK) return status;
            ++row;
        }
    }
    status = print_border(out, widths, columns);
    if (status != DB_OK) return status;
    if (has_limit && total > shown) {
        if (fprintf(out, "%llu records shown, %llu remaining.\n",
                (unsigned long long)shown,
                (unsigned long long)(total - shown)) < 0) return DB_ERR_IO;
    } else if (fprintf(out, "%llu record%s.\n",
            (unsigned long long)total, total == 1 ? "" : "s") < 0) {
        return DB_ERR_IO;
    }
    return DB_OK;
}

static DbStatus print_columns(Db *db, FILE *out)
{
    uint32_t i;
    if (fputs("Columns:", out) == EOF) return DB_ERR_IO;
    for (i = 0; i < storage_column_count(db->storage); ++i) {
        if (fprintf(out, "%s%s", i == 0 ? " " : ", ",
                storage_column_name(db->storage, i)) < 0) return DB_ERR_IO;
    }
    return fputc('\n', out) == EOF ? DB_ERR_IO : DB_OK;
}

static DbStatus print_help(FILE *out)
{
    static const char help[] =
        "Commands:\n"
        "  INSERT <tag> <value>...   (one value for each non-tag column)\n"
        "  SELECT <tag>\n"
        "  SELECT RANGE <low-tag> <high-tag>\n"
        "  UPDATE <tag> <column> <value>\n"
        "  DELETE <tag>\n"
        "  DISPLAY [LIMIT <n>]\n"
        "  ADD COLUMN <name>\n"
        "  RENAME COLUMN <old-name> <new-name>\n"
        "  COLUMNS\n"
        "  HELP\n"
        "  EXIT\n"
        "Quote values containing spaces; use \"\" for an empty cell.\n";
    return fputs(help, out) == EOF ? DB_ERR_IO : DB_OK;
}

DbStatus db_execute(Db *db, const char *line, FILE *out, int *exit_requested)
{
    Command command;
    DbStatus status;

    if (db == NULL || line == NULL || out == NULL) return DB_ERR_INVALID;
    if (exit_requested != NULL) *exit_requested = 0;
    status = command_parse(line, &command);
    if (status != DB_OK || command.type == CMD_NOOP) return status;

    switch (command.type) {
    case CMD_INSERT: {
        Record record;
        RecordRef existing;
        RecordRef ref;
        uint32_t i;
        if (command.value_count != storage_column_count(db->storage)) {
            return DB_ERR_SYNTAX;
        }
        memset(&record, 0, sizeof(record));
        for (i = 0; i < command.value_count; ++i) {
            if (strlen(command.values[i]) >= MAX_VALUE) return DB_ERR_INVALID;
            strcpy(record.values[i], command.values[i]);
        }
        status = record_validate(&record, storage_column_count(db->storage));
        if (status != DB_OK) return status;
        status = bptree_search(db->tree, record.values[0], &existing);
        if (status == DB_OK) return DB_ERR_DUPLICATE;
        if (status != DB_ERR_NOT_FOUND) return status;
        status = storage_append_record(db->storage, &record, &ref);
        if (status != DB_OK) return status;
        status = bptree_insert(db->tree, record.values[0], ref);
        if (status != DB_OK) {
            DbStatus rollback = storage_mark_deleted(db->storage, ref);
            return rollback == DB_OK ? status : DB_ERR_INTERNAL;
        }
        if (check_tree_after_mutation(db->tree) != DB_OK) return DB_ERR_INTERNAL;
        return fprintf(out, "1 record inserted.\n") < 0 ? DB_ERR_IO : DB_OK;
    }
    case CMD_SELECT:
        return display_records(db, out, 0, NULL, NULL,
            command.values[0], 0, 0);
    case CMD_SELECT_RANGE:
        return display_records(db, out, 1, command.values[0],
            command.values[1], NULL, 0, 0);
    case CMD_UPDATE: {
        RecordRef ref;
        Record record;
        int column;
        status = bptree_search(db->tree, command.values[0], &ref);
        if (status != DB_OK) return status;
        status = verify_reference(db, command.values[0], ref, &record);
        if (status != DB_OK) return status;
        column = storage_find_column(db->storage, command.column);
        if (column < 0) return DB_ERR_NOT_FOUND;
        if (column == 0) return DB_ERR_INVALID;
        if (strlen(command.values[1]) >= MAX_VALUE) return DB_ERR_INVALID;
        memset(record.values[column], 0, MAX_VALUE);
        strcpy(record.values[column], command.values[1]);
        status = record_validate(&record, storage_column_count(db->storage));
        if (status != DB_OK) return status;
        status = storage_write_record(db->storage, ref, &record);
        if (status != DB_OK) return status;
        return fprintf(out, "1 record updated.\n") < 0 ? DB_ERR_IO : DB_OK;
    }
    case CMD_DELETE: {
        RecordRef ref;
        status = bptree_search(db->tree, command.values[0], &ref);
        if (status != DB_OK) return status;
        status = storage_mark_deleted(db->storage, ref);
        if (status != DB_OK) return status;
        status = bptree_delete(db->tree, command.values[0]);
        if (status != DB_OK || check_tree_after_mutation(db->tree) != DB_OK) {
            return DB_ERR_INTERNAL;
        }
        return fprintf(out, "1 record deleted.\n") < 0 ? DB_ERR_IO : DB_OK;
    }
    case CMD_DISPLAY:
        return display_records(db, out, 0, NULL, NULL, NULL,
            command.has_limit, command.limit);
    case CMD_ADD_COLUMN:
        status = storage_add_column(db->storage, command.column);
        if (status != DB_OK) return status;
        if (fprintf(out, "Column added: %s.\n", command.column) < 0) return DB_ERR_IO;
        return DB_OK;
    case CMD_RENAME_COLUMN:
        status = storage_rename_column(db->storage, command.column,
            command.new_column);
        if (status != DB_OK) return status;
        if (fprintf(out, "Column renamed: %s -> %s.\n",
                command.column, command.new_column) < 0) return DB_ERR_IO;
        return DB_OK;
    case CMD_COLUMNS:
        return print_columns(db, out);
    case CMD_HELP:
        return print_help(out);
    case CMD_EXIT:
        if (exit_requested != NULL) *exit_requested = 1;
        return DB_OK;
    case CMD_NOOP:
        return DB_OK;
    default:
        return DB_ERR_INTERNAL;
    }
}

DbStatus db_close(Db *db)
{
    DbStatus status;
    if (db == NULL) return DB_OK;
    bptree_destroy(db->tree);
    status = storage_close(db->storage);
    free(db);
    return status;
}

void db_abort(Db *db)
{
    if (db == NULL) return;
    bptree_destroy(db->tree);
    storage_abort(db->storage);
    free(db);
}

unsigned int db_live_count(const Db *db)
{
    return db == NULL ? 0 : storage_live_count(db->storage);
}
