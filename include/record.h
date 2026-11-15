#ifndef MINIDB_RECORD_H
#define MINIDB_RECORD_H

#include "error.h"

#include <stdint.h>

#define MAX_COLUMNS 32
#define MAX_COLUMN_NAME 64
#define MAX_TAG_LENGTH 64
#define MAX_VALUE 256
#define RECORD_DISK_SIZE (4 + MAX_COLUMNS * MAX_VALUE)

typedef struct {
    char values[MAX_COLUMNS][MAX_VALUE];
    uint8_t deleted;
} Record;

DbStatus record_validate(const Record *record, uint32_t column_count);
DbStatus record_serialize(const Record *record, uint8_t output[RECORD_DISK_SIZE]);
DbStatus record_deserialize(const uint8_t input[RECORD_DISK_SIZE],
    uint32_t column_count, Record *record);
int record_valid_column_name(const char *name);

#endif
