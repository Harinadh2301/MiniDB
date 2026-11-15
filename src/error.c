#include "error.h"

const char *db_strerror(DbStatus status)
{
    switch (status) {
    case DB_OK: return "success";
    case DB_ERR_SYNTAX: return "invalid command syntax";
    case DB_ERR_INVALID: return "invalid value";
    case DB_ERR_DUPLICATE: return "duplicate ID";
    case DB_ERR_NOT_FOUND: return "record not found";
    case DB_ERR_IO: return "database I/O error";
    case DB_ERR_NOMEM: return "out of memory";
    case DB_ERR_CORRUPT: return "database file is corrupt or unsupported";
    case DB_ERR_LOCKED: return "database is locked by another process";
    case DB_ERR_INTERNAL: return "internal database consistency error";
    default: return "unknown database error";
    }
}
