#ifndef MINIDB_QUERY_H
#define MINIDB_QUERY_H

#include "error.h"

#include <stdio.h>

typedef struct Db Db;

DbStatus db_open(const char *path, Db **out, int *was_dirty);
DbStatus db_execute(Db *db, const char *line, FILE *out, int *exit_requested);
DbStatus db_close(Db *db);
void db_abort(Db *db);
unsigned int db_live_count(const Db *db);

#endif
