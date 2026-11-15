#include "cli.h"
#include "query.h"
#include "storage.h"

#include <stdio.h>

#ifdef _WIN32
#include <io.h>
extern int _fileno(FILE *file);
#define input_is_terminal() (_isatty(_fileno(stdin)) != 0)
#else
#include <unistd.h>
#define input_is_terminal() (isatty(STDIN_FILENO) != 0)
#endif

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "temp_data.db";
    Db *db = NULL;
    int was_dirty = 0;
    int interactive;
    int exit_requested = 0;
    DbStatus status;

    if (argc > 2) {
        fprintf(stderr, "Usage: %s [database-file]\n", argv[0]);
        return 2;
    }
    interactive = input_is_terminal();
    status = db_open(path, &db, &was_dirty);
    if (status != DB_OK) {
        if (status == DB_ERR_LOCKED) {
            fprintf(stderr, "Error: %s. If this is a stale lock, remove %s.lock and retry.\n",
                db_strerror(status), path);
        } else {
            fprintf(stderr, "Error opening %s: %s.\n", path, db_strerror(status));
        }
        return 1;
    }
    if (was_dirty) {
        fprintf(stderr, "Warning: previous shutdown was unclean; the index was rebuilt from records.\n");
    }
    if (interactive) {
        printf("MiniDB v1.1\n");
        printf("Database opened: %s (%u records)\n", path, db_live_count(db));
    }

    status = cli_run(db, stdin, stdout, interactive, &exit_requested);
    if (status != DB_OK) {
        fprintf(stderr, "MiniDB stopped: %s.\n", db_strerror(status));
        db_abort(db);
        return 1;
    }
    status = db_close(db);
    if (status != DB_OK) {
        fprintf(stderr, "Error closing database: %s.\n", db_strerror(status));
        return 1;
    }
    if (exit_requested && fputs("Database closed.\n", stdout) == EOF) return 1;
    return 0;
}
