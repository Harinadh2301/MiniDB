#include "cli.h"

#include "parser.h"

#include <string.h>

#define INPUT_BUFFER_SIZE 16384

static int only_space(const char *text)
{
    while (*text != '\0') {
        if (*text != ' ' && *text != '\t' && *text != '\r'
            && *text != '\n' && *text != '\v' && *text != '\f') return 0;
        ++text;
    }
    return 1;
}

static int is_script_comment(const char *text)
{
    while (*text == ' ' || *text == '\t' || *text == '\r'
        || *text == '\n' || *text == '\v' || *text == '\f') ++text;
    return *text == '#';
}

DbStatus cli_run(Db *db, FILE *input, FILE *output, int interactive,
    int *did_exit)
{
    char line[INPUT_BUFFER_SIZE];

    if (db == NULL || input == NULL || output == NULL) return DB_ERR_INVALID;
    if (did_exit != NULL) *did_exit = 0;
    for (;;) {
        size_t length;
        int command_exit = 0;
        DbStatus status;

        if (interactive) {
            if (fputs("MiniDB> ", output) == EOF || fflush(output) != 0) {
                return DB_ERR_IO;
            }
        }
        if (fgets(line, sizeof(line), input) == NULL) {
            if (ferror(input)) return DB_ERR_IO;
            return DB_OK;
        }

        length = strlen(line);
        if (length > 0 && line[length - 1] != '\n' && !feof(input)) {
            int next = fgetc(input);
            if (next != '\n' && next != EOF) {
                while (next != '\n' && next != EOF) next = fgetc(input);
                if (fputs("Error: command line is too long.\n", output) == EOF) {
                    return DB_ERR_IO;
                }
                continue;
            }
        }
        if (length > 0 && line[length - 1] == '\n') line[length - 1] = '\0';
        length = strlen(line);
        if (length > 0 && line[length - 1] == '\r') line[length - 1] = '\0';
        if (only_space(line) || (!interactive && is_script_comment(line))) continue;

        status = db_execute(db, line, output, &command_exit);
        if (status != DB_OK) {
            if (fprintf(output, "Error: %s.\n", db_strerror(status)) < 0) {
                return DB_ERR_IO;
            }
            if (status == DB_ERR_IO || status == DB_ERR_INTERNAL
                || status == DB_ERR_CORRUPT || status == DB_ERR_NOMEM) {
                return status;
            }
            continue;
        }
        if (command_exit) {
            if (did_exit != NULL) *did_exit = 1;
            return DB_OK;
        }
    }
}
