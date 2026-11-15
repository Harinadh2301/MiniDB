#ifndef MINIDB_ERROR_H
#define MINIDB_ERROR_H

typedef enum {
    DB_OK = 0,
    DB_ERR_SYNTAX,
    DB_ERR_INVALID,
    DB_ERR_DUPLICATE,
    DB_ERR_NOT_FOUND,
    DB_ERR_IO,
    DB_ERR_NOMEM,
    DB_ERR_CORRUPT,
    DB_ERR_LOCKED,
    DB_ERR_INTERNAL
} DbStatus;

const char *db_strerror(DbStatus status);

#endif
