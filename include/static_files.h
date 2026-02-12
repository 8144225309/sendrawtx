#ifndef STATIC_FILES_H
#define STATIC_FILES_H

#include <stddef.h>
#include "config.h"

/*
 * Static file loaded into memory.
 * Content is heap-allocated and must be freed.
 */
typedef struct StaticFile {
    char *content;           /* File contents (null-terminated) */
    size_t length;           /* Content length in bytes */
    const char *content_type; /* MIME type for HTTP header */
} StaticFile;

/*
 * Collection of all static files needed by the server.
 * Each worker loads its own copy (no locking needed).
 */
typedef struct StaticFiles {
    StaticFile index;        /* index.html - home/welcome page */
    StaticFile broadcast;    /* broadcast.html - raw tx submission */
    StaticFile result;       /* result.html - txid status page */
    StaticFile error;        /* error.html - error page */
    StaticFile docs;         /* docs.html - API documentation */
    StaticFile status;       /* status.html - system status */
    StaticFile logos;        /* logos.html - logo showcase */
} StaticFiles;

/*
 * Load all static files from a directory.
 * Files are served as-is with no modification.
 *
 * @param files  Pointer to StaticFiles struct to populate
 * @param dir    Directory path (e.g., "./static")
 * @param config Config (reserved for future use)
 * @return 0 on success, -1 on error
 */
int static_files_load(StaticFiles *files, const char *dir, const Config *config);

/*
 * Free all loaded static files.
 *
 * @param files Pointer to StaticFiles struct to free
 */
void static_files_free(StaticFiles *files);

#endif /* STATIC_FILES_H */
