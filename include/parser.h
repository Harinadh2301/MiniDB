#ifndef MINIDB_PARSER_H
#define MINIDB_PARSER_H

#include "error.h"
#include "record.h"

#include <stdint.h>

typedef enum {
    CMD_NOOP = 0,
    CMD_INSERT,
    CMD_SELECT,
    CMD_SELECT_RANGE,
    CMD_UPDATE,
    CMD_DELETE,
    CMD_DISPLAY,
    CMD_ADD_COLUMN,
    CMD_RENAME_COLUMN,
    CMD_COLUMNS,
    CMD_HELP,
    CMD_EXIT
} CommandType;

typedef struct {
    CommandType type;
    uint32_t limit;
    int has_limit;
    uint32_t value_count;
    char values[MAX_COLUMNS][MAX_VALUE];
    char column[MAX_COLUMN_NAME];
    char new_column[MAX_COLUMN_NAME];
} Command;

DbStatus command_parse(const char *line, Command *command);

#endif
