#include "parser.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define TOKEN_CAPACITY (MAX_COLUMNS + 4)
#define COMMAND_LINE_CAPACITY 8192

static int equal_word(const char *left, const char *right)
{
    while (*left != '\0' && *right != '\0') {
        unsigned char a = (unsigned char)*left++;
        unsigned char b = (unsigned char)*right++;
        if (a >= 'a' && a <= 'z') a = (unsigned char)(a - 'a' + 'A');
        if (b >= 'a' && b <= 'z') b = (unsigned char)(b - 'a' + 'A');
        if (a != b) return 0;
    }
    return *left == '\0' && *right == '\0';
}

static int is_space(char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n'
        || ch == '\v' || ch == '\f';
}

static DbStatus tokenize(const char *line, char tokens[TOKEN_CAPACITY][MAX_VALUE],
    size_t *token_count)
{
    const char *cursor = line;
    *token_count = 0;
    if (strlen(line) >= COMMAND_LINE_CAPACITY) return DB_ERR_SYNTAX;

    while (*cursor != '\0') {
        size_t length = 0;
        int quoted = 0;
        int closed = 0;

        while (is_space(*cursor)) ++cursor;
        if (*cursor == '\0') break;
        if (*token_count >= TOKEN_CAPACITY) return DB_ERR_SYNTAX;
        if (*cursor == '"') {
            quoted = 1;
            ++cursor;
        }
        while (*cursor != '\0') {
            char ch = *cursor++;
            if (quoted) {
                if (ch == '"') {
                    closed = 1;
                    break;
                }
                if (ch == '\\' && (*cursor == '"' || *cursor == '\\')) {
                    ch = *cursor++;
                }
            } else if (is_space(ch)) {
                break;
            }
            if (length + 1 >= MAX_VALUE) return DB_ERR_INVALID;
            if ((unsigned char)ch < 0x20 && ch != '\t') return DB_ERR_INVALID;
            tokens[*token_count][length++] = ch;
        }
        if (quoted && !closed) return DB_ERR_SYNTAX;
        if (quoted && *cursor != '\0' && !is_space(*cursor)) return DB_ERR_SYNTAX;
        tokens[*token_count][length] = '\0';
        ++*token_count;
    }
    return DB_OK;
}

static DbStatus parse_limit(const char *text, uint32_t *out)
{
    char *end;
    unsigned long value;
    if (*text == '-') return DB_ERR_INVALID;
    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno == ERANGE || end == text || *end != '\0' || value > UINT32_MAX) {
        return DB_ERR_INVALID;
    }
    *out = (uint32_t)value;
    return DB_OK;
}

DbStatus command_parse(const char *line, Command *command)
{
    char tokens[TOKEN_CAPACITY][MAX_VALUE];
    size_t count;
    size_t i;
    DbStatus status;

    if (line == NULL || command == NULL) return DB_ERR_INVALID;
    memset(command, 0, sizeof(*command));
    status = tokenize(line, tokens, &count);
    if (status != DB_OK || count == 0) return status;

    if (equal_word(tokens[0], "INSERT")) {
        if (count < 3 || count > MAX_COLUMNS + 1) return DB_ERR_SYNTAX;
        command->type = CMD_INSERT;
        command->value_count = (uint32_t)(count - 1);
        for (i = 1; i < count; ++i) {
            memcpy(command->values[i - 1], tokens[i], strlen(tokens[i]) + 1);
        }
        return DB_OK;
    }
    if (equal_word(tokens[0], "SELECT")) {
        if (count == 2) {
            command->type = CMD_SELECT;
            strcpy(command->values[0], tokens[1]);
            command->value_count = 1;
            return DB_OK;
        }
        if (count == 4 && equal_word(tokens[1], "RANGE")) {
            command->type = CMD_SELECT_RANGE;
            strcpy(command->values[0], tokens[2]);
            strcpy(command->values[1], tokens[3]);
            command->value_count = 2;
            return DB_OK;
        }
        return DB_ERR_SYNTAX;
    }
    if (equal_word(tokens[0], "UPDATE")) {
        if (count != 4) return DB_ERR_SYNTAX;
        command->type = CMD_UPDATE;
        strcpy(command->values[0], tokens[1]);
        strcpy(command->column, tokens[2]);
        strcpy(command->values[1], tokens[3]);
        command->value_count = 2;
        return DB_OK;
    }
    if (equal_word(tokens[0], "DELETE")) {
        if (count != 2) return DB_ERR_SYNTAX;
        command->type = CMD_DELETE;
        strcpy(command->values[0], tokens[1]);
        command->value_count = 1;
        return DB_OK;
    }
    if (equal_word(tokens[0], "DISPLAY")) {
        if (count == 1) {
            command->type = CMD_DISPLAY;
            return DB_OK;
        }
        if (count == 3 && equal_word(tokens[1], "LIMIT")) {
            command->type = CMD_DISPLAY;
            command->has_limit = 1;
            return parse_limit(tokens[2], &command->limit);
        }
        return DB_ERR_SYNTAX;
    }
    if (equal_word(tokens[0], "ADD")) {
        if (count != 3 || !equal_word(tokens[1], "COLUMN")
            || strlen(tokens[2]) >= MAX_COLUMN_NAME) return DB_ERR_SYNTAX;
        command->type = CMD_ADD_COLUMN;
        strcpy(command->column, tokens[2]);
        return DB_OK;
    }
    if (equal_word(tokens[0], "RENAME")) {
        if (count != 4 || !equal_word(tokens[1], "COLUMN")
            || strlen(tokens[2]) >= MAX_COLUMN_NAME
            || strlen(tokens[3]) >= MAX_COLUMN_NAME) return DB_ERR_SYNTAX;
        command->type = CMD_RENAME_COLUMN;
        strcpy(command->column, tokens[2]);
        strcpy(command->new_column, tokens[3]);
        return DB_OK;
    }
    if (equal_word(tokens[0], "COLUMNS")) {
        if (count != 1) return DB_ERR_SYNTAX;
        command->type = CMD_COLUMNS;
        return DB_OK;
    }
    if (equal_word(tokens[0], "HELP")) {
        if (count != 1) return DB_ERR_SYNTAX;
        command->type = CMD_HELP;
        return DB_OK;
    }
    if (equal_word(tokens[0], "EXIT")) {
        if (count != 1) return DB_ERR_SYNTAX;
        command->type = CMD_EXIT;
        return DB_OK;
    }
    return DB_ERR_SYNTAX;
}
