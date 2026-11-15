#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L

#include "storage.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#define open _open
#define close _close
#define write _write
#define unlink _unlink
#define LOCK_FLAGS (_O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY)
#define LOCK_MODE (_S_IREAD | _S_IWRITE)
#ifdef __MINGW32__
extern int fseeko64(FILE *file, long long offset, int origin);
extern long long ftello64(FILE *file);
#else
extern int _fseeki64(FILE *file, long long offset, int origin);
extern long long _ftelli64(FILE *file);
#endif
extern int _fileno(FILE *file);
#else
#include <fcntl.h>
#include <unistd.h>
#define LOCK_FLAGS (O_CREAT | O_EXCL | O_WRONLY)
#define LOCK_MODE 0600
#endif

#define FLAG_DIRTY 1u
#define FREE_LIST_NONE UINT32_MAX
#define HEADER_COLUMN_COUNT_OFFSET 44
#define HEADER_COLUMNS_OFFSET 48

struct Storage {
    FILE *file;
    char *lock_path;
    uint32_t slot_count;
    uint32_t live_count;
    uint32_t flags;
    uint32_t column_count;
    char columns[MAX_COLUMNS][MAX_COLUMN_NAME];
};

static void put_u32_le(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
    out[2] = (uint8_t)(value >> 16);
    out[3] = (uint8_t)(value >> 24);
}

static uint32_t get_u32_le(const uint8_t *in)
{
    return (uint32_t)in[0]
        | ((uint32_t)in[1] << 8)
        | ((uint32_t)in[2] << 16)
        | ((uint32_t)in[3] << 24);
}

static int seek_file(FILE *file, int64_t offset, int origin)
{
#ifdef _WIN32
#ifdef __MINGW32__
    return fseeko64(file, (long long)offset, origin);
#else
    return _fseeki64(file, offset, origin);
#endif
#else
    return fseeko(file, (off_t)offset, origin);
#endif
}

static int64_t tell_file(FILE *file)
{
#ifdef _WIN32
#ifdef __MINGW32__
    return (int64_t)ftello64(file);
#else
    return _ftelli64(file);
#endif
#else
    return (int64_t)ftello(file);
#endif
}

static int durable_flush(FILE *file)
{
    if (fflush(file) != 0) return -1;
#ifdef _WIN32
    return _commit(_fileno(file));
#else
    return fsync(fileno(file));
#endif
}

static int64_t record_position(uint32_t slot)
{
    return (int64_t)DB_HEADER_SIZE
        + (int64_t)slot * (int64_t)RECORD_DISK_SIZE;
}

static void copy_column_name(uint8_t *header, uint32_t index, const char *name)
{
    memcpy(header + HEADER_COLUMNS_OFFSET + index * MAX_COLUMN_NAME,
        name, strlen(name));
}

static DbStatus write_header(Storage *storage)
{
    uint8_t header[DB_HEADER_SIZE];
    uint32_t i;

    memset(header, 0, sizeof(header));
    memcpy(header, "MINIDB\0\0", 8);
    put_u32_le(header + 8, DB_FORMAT_VERSION);
    put_u32_le(header + 12, DB_PAGE_SIZE);
    put_u32_le(header + 16, RECORD_DISK_SIZE);
    put_u32_le(header + 20, storage->slot_count);
    put_u32_le(header + 24, storage->live_count);
    put_u32_le(header + 28, storage->flags);
    put_u32_le(header + 32, FREE_LIST_NONE);
    put_u32_le(header + HEADER_COLUMN_COUNT_OFFSET, storage->column_count);
    for (i = 0; i < storage->column_count; ++i) {
        copy_column_name(header, i, storage->columns[i]);
    }

    if (seek_file(storage->file, 0, SEEK_SET) != 0
        || fwrite(header, sizeof(header), 1, storage->file) != 1
        || fflush(storage->file) != 0) {
        return DB_ERR_IO;
    }
    return DB_OK;
}

static int release_storage(Storage *storage, int remove_lock)
{
    int failed = 0;
    if (storage == NULL) return 0;
    if (storage->file != NULL && fclose(storage->file) != 0) failed = 1;
    if (remove_lock && storage->lock_path != NULL) {
        if (unlink(storage->lock_path) != 0 && errno != ENOENT) failed = 1;
    }
    free(storage->lock_path);
    free(storage);
    return failed ? -1 : 0;
}

static DbStatus acquire_lock(Storage *storage, const char *path)
{
    size_t length = strlen(path);
    int descriptor;
    const char message[] = "locked\n";

    if (length > SIZE_MAX - 6) return DB_ERR_INVALID;
    storage->lock_path = (char *)malloc(length + 6);
    if (storage->lock_path == NULL) return DB_ERR_NOMEM;
    memcpy(storage->lock_path, path, length);
    memcpy(storage->lock_path + length, ".lock", 6);
    descriptor = open(storage->lock_path, LOCK_FLAGS, LOCK_MODE);
    if (descriptor < 0) return errno == EEXIST ? DB_ERR_LOCKED : DB_ERR_IO;
    if (write(descriptor, message, (unsigned int)(sizeof(message) - 1))
        != (int)(sizeof(message) - 1)) {
        close(descriptor);
        unlink(storage->lock_path);
        return DB_ERR_IO;
    }
    if (close(descriptor) != 0) {
        unlink(storage->lock_path);
        return DB_ERR_IO;
    }
    return DB_OK;
}

static DbStatus initialize_file(Storage *storage)
{
    storage->slot_count = 0;
    storage->live_count = 0;
    storage->flags = FLAG_DIRTY;
    storage->column_count = 2;
    strcpy(storage->columns[0], "tag");
    strcpy(storage->columns[1], "data");
    if (write_header(storage) != DB_OK) return DB_ERR_IO;
    return durable_flush(storage->file) == 0 ? DB_OK : DB_ERR_IO;
}

static int equal_column_name(const char *left, const char *right)
{
    while (*left != '\0' && *right != '\0') {
        unsigned char a = (unsigned char)*left++;
        unsigned char b = (unsigned char)*right++;
        if (a >= 'A' && a <= 'Z') a = (unsigned char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (unsigned char)(b - 'A' + 'a');
        if (a != b) return 0;
    }
    return *left == '\0' && *right == '\0';
}

DbStatus storage_open(const char *path, Storage **out, int *was_dirty)
{
    Storage *storage;
    struct stat info;
    uint8_t header[DB_HEADER_SIZE];
    int exists;
    int64_t file_size;
    int64_t required_size;
    uint32_t i;
    DbStatus status;

    if (path == NULL || out == NULL) return DB_ERR_INVALID;
    *out = NULL;
    if (was_dirty != NULL) *was_dirty = 0;
    storage = (Storage *)calloc(1, sizeof(*storage));
    if (storage == NULL) return DB_ERR_NOMEM;
    status = acquire_lock(storage, path);
    if (status != DB_OK) {
        release_storage(storage, status != DB_ERR_LOCKED);
        return status;
    }

    if (stat(path, &info) == 0) {
        exists = 1;
    } else if (errno == ENOENT) {
        exists = 0;
    } else {
        release_storage(storage, 1);
        return DB_ERR_IO;
    }
    storage->file = fopen(path, exists ? "r+b" : "w+b");
    if (storage->file == NULL) {
        release_storage(storage, 1);
        return DB_ERR_IO;
    }
    if (!exists) {
        status = initialize_file(storage);
        if (status != DB_OK) {
            release_storage(storage, 1);
            return status;
        }
    } else {
        if (fread(header, sizeof(header), 1, storage->file) != 1
            || memcmp(header, "MINIDB\0\0", 8) != 0
            || get_u32_le(header + 8) != DB_FORMAT_VERSION
            || get_u32_le(header + 12) != DB_PAGE_SIZE
            || get_u32_le(header + 16) != RECORD_DISK_SIZE) {
            release_storage(storage, 1);
            return DB_ERR_CORRUPT;
        }
        storage->slot_count = get_u32_le(header + 20);
        storage->live_count = get_u32_le(header + 24);
        storage->flags = get_u32_le(header + 28);
        storage->column_count = get_u32_le(header + HEADER_COLUMN_COUNT_OFFSET);
        if (storage->live_count > storage->slot_count
            || (storage->flags & ~FLAG_DIRTY) != 0
            || get_u32_le(header + 32) != FREE_LIST_NONE
            || storage->column_count < 2 || storage->column_count > MAX_COLUMNS
            || get_u32_le(header + 36) != 0 || get_u32_le(header + 40) != 0) {
            release_storage(storage, 1);
            return DB_ERR_CORRUPT;
        }
        for (i = 0; i < storage->column_count; ++i) {
            const uint8_t *bytes = header + HEADER_COLUMNS_OFFSET + i * MAX_COLUMN_NAME;
            size_t length = 0;
            size_t j;
            while (length < MAX_COLUMN_NAME && bytes[length] != 0) ++length;
            if (length == MAX_COLUMN_NAME) {
                release_storage(storage, 1);
                return DB_ERR_CORRUPT;
            }
            memcpy(storage->columns[i], bytes, length);
            storage->columns[i][length] = '\0';
            if (!record_valid_column_name(storage->columns[i])) {
                release_storage(storage, 1);
                return DB_ERR_CORRUPT;
            }
            for (j = 0; j < i; ++j) {
                if (equal_column_name(storage->columns[j], storage->columns[i])) {
                    release_storage(storage, 1);
                    return DB_ERR_CORRUPT;
                }
            }
        }
        for (i = HEADER_COLUMNS_OFFSET
                + storage->column_count * MAX_COLUMN_NAME;
            i < DB_HEADER_SIZE; ++i) {
            if (header[i] != 0) {
                release_storage(storage, 1);
                return DB_ERR_CORRUPT;
            }
        }
        if (strcmp(storage->columns[0], "tag") != 0) {
            if (!record_valid_column_name(storage->columns[0])) {
                release_storage(storage, 1);
                return DB_ERR_CORRUPT;
            }
        }
        if (was_dirty != NULL) *was_dirty = (storage->flags & FLAG_DIRTY) != 0;
        if (seek_file(storage->file, 0, SEEK_END) != 0
            || (file_size = tell_file(storage->file)) < 0) {
            release_storage(storage, 1);
            return DB_ERR_IO;
        }
        required_size = (int64_t)DB_HEADER_SIZE
            + (int64_t)storage->slot_count * RECORD_DISK_SIZE;
        if (file_size < required_size) {
            release_storage(storage, 1);
            return DB_ERR_CORRUPT;
        }
        storage->flags |= FLAG_DIRTY;
        status = write_header(storage);
        if (status != DB_OK || durable_flush(storage->file) != 0) {
            release_storage(storage, 1);
            return DB_ERR_IO;
        }
    }
    *out = storage;
    return DB_OK;
}

DbStatus storage_append_record(Storage *storage, const Record *record, RecordRef *out)
{
    uint8_t bytes[RECORD_DISK_SIZE];
    DbStatus status;
    uint32_t slot;

    if (storage == NULL || out == NULL || record == NULL) return DB_ERR_INVALID;
    if (storage->slot_count == UINT32_MAX) return DB_ERR_IO;
    if (record->deleted != 0) return DB_ERR_INVALID;
    status = record_validate(record, storage->column_count);
    if (status != DB_OK) return status;
    status = record_serialize(record, bytes);
    if (status != DB_OK) return status;
    slot = storage->slot_count;
    if (seek_file(storage->file, record_position(slot), SEEK_SET) != 0
        || fwrite(bytes, sizeof(bytes), 1, storage->file) != 1) {
        return DB_ERR_IO;
    }
    ++storage->slot_count;
    ++storage->live_count;
    status = write_header(storage);
    if (status != DB_OK) return status;
    out->slot = slot;
    return DB_OK;
}

DbStatus storage_read_record(Storage *storage, RecordRef ref, Record *out)
{
    uint8_t bytes[RECORD_DISK_SIZE];
    if (storage == NULL || out == NULL) return DB_ERR_INVALID;
    if (ref.slot >= storage->slot_count) return DB_ERR_INTERNAL;
    if (seek_file(storage->file, record_position(ref.slot), SEEK_SET) != 0
        || fread(bytes, sizeof(bytes), 1, storage->file) != 1) {
        return DB_ERR_IO;
    }
    return record_deserialize(bytes, storage->column_count, out);
}

DbStatus storage_write_record(Storage *storage, RecordRef ref, const Record *record)
{
    uint8_t bytes[RECORD_DISK_SIZE];
    DbStatus status;
    if (storage == NULL || record == NULL) return DB_ERR_INVALID;
    if (ref.slot >= storage->slot_count) return DB_ERR_INTERNAL;
    status = record_validate(record, storage->column_count);
    if (status != DB_OK) return status;
    status = record_serialize(record, bytes);
    if (status != DB_OK) return status;
    if (seek_file(storage->file, record_position(ref.slot), SEEK_SET) != 0
        || fwrite(bytes, sizeof(bytes), 1, storage->file) != 1
        || fflush(storage->file) != 0) {
        return DB_ERR_IO;
    }
    return DB_OK;
}

DbStatus storage_mark_deleted(Storage *storage, RecordRef ref)
{
    Record record;
    DbStatus status;
    if (storage == NULL) return DB_ERR_INVALID;
    status = storage_read_record(storage, ref, &record);
    if (status != DB_OK) return status;
    if (record.deleted != 0 || storage->live_count == 0) return DB_ERR_INTERNAL;
    record.deleted = 1;
    status = storage_write_record(storage, ref, &record);
    if (status != DB_OK) return status;
    --storage->live_count;
    return write_header(storage);
}

DbStatus storage_add_column(Storage *storage, const char *name)
{
    if (storage == NULL || !record_valid_column_name(name)) return DB_ERR_INVALID;
    if (storage->column_count >= MAX_COLUMNS) return DB_ERR_INVALID;
    if (storage_find_column(storage, name) >= 0) return DB_ERR_DUPLICATE;
    strcpy(storage->columns[storage->column_count], name);
    ++storage->column_count;
    if (write_header(storage) != DB_OK) {
        --storage->column_count;
        storage->columns[storage->column_count][0] = '\0';
        return DB_ERR_IO;
    }
    return DB_OK;
}

DbStatus storage_rename_column(Storage *storage, const char *old_name,
    const char *new_name)
{
    int index;
    char previous[MAX_COLUMN_NAME];
    if (storage == NULL || !record_valid_column_name(new_name)) return DB_ERR_INVALID;
    index = storage_find_column(storage, old_name);
    if (index < 0) return DB_ERR_NOT_FOUND;
    if (storage_find_column(storage, new_name) >= 0) return DB_ERR_DUPLICATE;
    strcpy(previous, storage->columns[index]);
    strcpy(storage->columns[index], new_name);
    if (write_header(storage) != DB_OK) {
        strcpy(storage->columns[index], previous);
        return DB_ERR_IO;
    }
    return DB_OK;
}

uint32_t storage_column_count(const Storage *storage)
{
    return storage == NULL ? 0 : storage->column_count;
}

const char *storage_column_name(const Storage *storage, uint32_t index)
{
    if (storage == NULL || index >= storage->column_count) return NULL;
    return storage->columns[index];
}

int storage_find_column(const Storage *storage, const char *name)
{
    uint32_t i;
    if (storage == NULL || name == NULL) return -1;
    for (i = 0; i < storage->column_count; ++i) {
        if (equal_column_name(storage->columns[i], name)) return (int)i;
    }
    return -1;
}

uint32_t storage_slot_count(const Storage *storage)
{
    return storage == NULL ? 0 : storage->slot_count;
}

uint32_t storage_live_count(const Storage *storage)
{
    return storage == NULL ? 0 : storage->live_count;
}

DbStatus storage_reconcile_live_count(Storage *storage, uint32_t live_count)
{
    if (storage == NULL || live_count > storage->slot_count) return DB_ERR_INVALID;
    storage->live_count = live_count;
    return write_header(storage);
}

DbStatus storage_close(Storage *storage)
{
    DbStatus status;
    if (storage == NULL) return DB_OK;
    storage->flags &= ~FLAG_DIRTY;
    status = write_header(storage);
    if (status == DB_OK && durable_flush(storage->file) != 0) status = DB_ERR_IO;
    if (release_storage(storage, 1) != 0) status = DB_ERR_IO;
    return status;
}

void storage_abort(Storage *storage)
{
    release_storage(storage, 1);
}
