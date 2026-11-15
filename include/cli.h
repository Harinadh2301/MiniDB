#ifndef MINIDB_CLI_H
#define MINIDB_CLI_H

#include "error.h"
#include "query.h"

#include <stdio.h>

DbStatus cli_run(Db *db, FILE *input, FILE *output, int interactive,
    int *exit_requested);

#endif
