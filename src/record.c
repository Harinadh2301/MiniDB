#include "record.h"

#include <string.h>

int record_valid_column_name(const char *name)
{
    size_t i;
    size_t length;

    if (name == NULL) return 0;
    length = strlen(name);
    if (length == 0 || length >= MAX_COLUMN_NAME) return 0;
    for (i = 0; i < length; ++i) {
        unsigned char ch = (unsigned char)name[i];
        if (ch < 0x20 || ch > 0x7e) return 0;
    }
    return 1;
}

DbStatus record_validate(const Record *record, uint32_t column_count)
{
    uint32_t i;
    const char *tag_end;

    if (record == NULL || column_count == 0 || column_count > MAX_COLUMNS
        || record->deleted > 1) {
        return DB_ERR_INVALID;
    }
    tag_end = (const char *)memchr(record->values[0], '\0', MAX_VALUE);
    if (tag_end == NULL || tag_end == record->values[0]
        || (size_t)(tag_end - record->values[0]) >= MAX_TAG_LENGTH) {
        return DB_ERR_INVALID;
    }
    for (i = 0; i < column_count; ++i) {
        size_t j;
        const char *end = (const char *)memchr(record->values[i], '\0', MAX_VALUE);
        size_t length;
        if (end == NULL) return DB_ERR_INVALID;
        length = (size_t)(end - record->values[i]);
        for (j = 0; j < length; ++j) {
            unsigned char ch = (unsigned char)record->values[i][j];
            if (ch < 0x20 || ch == 0x7f) return DB_ERR_INVALID;
        }
    }
    return DB_OK;
}

DbStatus record_serialize(const Record *record, uint8_t output[RECORD_DISK_SIZE])
{
    if (record == NULL || output == NULL || record->deleted > 1) {
        return DB_ERR_INVALID;
    }
    memset(output, 0, RECORD_DISK_SIZE);
    output[0] = record->deleted;
    memcpy(output + 4, record->values, sizeof(record->values));
    return DB_OK;
}

DbStatus record_deserialize(const uint8_t input[RECORD_DISK_SIZE],
    uint32_t column_count, Record *record)
{
    uint32_t i;

    if (input == NULL || record == NULL || input[0] > 1
        || input[1] != 0 || input[2] != 0 || input[3] != 0) {
        return DB_ERR_CORRUPT;
    }
    memset(record, 0, sizeof(*record));
    record->deleted = input[0];
    memcpy(record->values, input + 4, sizeof(record->values));
    for (i = 0; i < MAX_COLUMNS; ++i) {
        if (memchr(record->values[i], '\0', MAX_VALUE) == NULL) {
            return DB_ERR_CORRUPT;
        }
        if (i >= column_count) {
            size_t j;
            for (j = 0; j < MAX_VALUE; ++j) {
                if (record->values[i][j] != '\0') return DB_ERR_CORRUPT;
            }
        }
    }
    return record_validate(record, column_count) == DB_OK
        ? DB_OK : DB_ERR_CORRUPT;
}
