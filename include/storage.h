#ifndef MINIDB_STORAGE_H
#define MINIDB_STORAGE_H

#include "bptree.h"
#include "error.h"
#include "record.h"

#include <stdint.h>

#define DB_HEADER_SIZE 4096
#define DB_FORMAT_VERSION 2
#define DB_PAGE_SIZE 4096

typedef struct Storage Storage;

DbStatus storage_open(const char *path, Storage **out, int *was_dirty);
DbStatus storage_close(Storage *storage);
void storage_abort(Storage *storage);
DbStatus storage_append_record(Storage *storage, const Record *record, RecordRef *out);
DbStatus storage_read_record(Storage *storage, RecordRef ref, Record *out);
DbStatus storage_write_record(Storage *storage, RecordRef ref, const Record *record);
DbStatus storage_mark_deleted(Storage *storage, RecordRef ref);
DbStatus storage_add_column(Storage *storage, const char *name);
DbStatus storage_rename_column(Storage *storage, const char *old_name,
    const char *new_name);
uint32_t storage_column_count(const Storage *storage);
const char *storage_column_name(const Storage *storage, uint32_t index);
int storage_find_column(const Storage *storage, const char *name);
uint32_t storage_slot_count(const Storage *storage);
uint32_t storage_live_count(const Storage *storage);
DbStatus storage_reconcile_live_count(Storage *storage, uint32_t live_count);

#endif
