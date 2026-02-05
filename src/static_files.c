#include "static_files.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

/* Maximum file size (1MB should be plenty for HTML) */
#define MAX_FILE_SIZE (1024 * 1024)

/*
 * Load a single file into memory.
 * Returns 0 on success, -1 on error.
 */
static int load_file(StaticFile *file, const char *path, const char *content_type)
{
    struct stat st;
    int fd = -1;
    char *content = NULL;

    file->content = NULL;
    file->length = 0;
    file->content_type = content_type;

    /* Get file size */
    if (stat(path, &st) < 0) {
        log_error("Failed to stat %s: %s", path, strerror(errno));
        return -1;
    }

    if (st.st_size > MAX_FILE_SIZE) {
        log_error("File %s too large (%ld bytes, max %d)", path, (long)st.st_size, MAX_FILE_SIZE);
        return -1;
    }

    /* Allocate buffer (+1 for null terminator) */
    content = malloc(st.st_size + 1);
    if (!content) {
        log_error("Failed to allocate %ld bytes for %s", (long)st.st_size, path);
        return -1;
    }

    /* Open file */
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        log_error("Failed to open %s: %s", path, strerror(errno));
        free(content);
        return -1;
    }

    /* Read entire file */
    ssize_t total = 0;
    while (total < st.st_size) {
        ssize_t n = read(fd, content + total, st.st_size - total);
        if (n < 0) {
            if (errno == EINTR) continue;
            log_error("Failed to read %s: %s", path, strerror(errno));
            close(fd);
            free(content);
            return -1;
        }
        if (n == 0) break; /* EOF */
        total += n;
    }

    close(fd);

    /* Null-terminate */
    content[total] = '\0';

    file->content = content;
    file->length = (size_t)total;

    log_info("Loaded %s (%zu bytes)", path, file->length);
    return 0;
}

/*
 * Free a single file.
 */
static void free_file(StaticFile *file)
{
    if (file->content) {
        free(file->content);
        file->content = NULL;
    }
    file->length = 0;
}

int static_files_load(StaticFiles *files, const char *dir)
{
    char path[512];

    memset(files, 0, sizeof(StaticFiles));

    /* Load broadcast.html */
    snprintf(path, sizeof(path), "%s/broadcast.html", dir);
    if (load_file(&files->broadcast, path, "text/html; charset=utf-8") < 0) {
        static_files_free(files);
        return -1;
    }

    /* Load result.html */
    snprintf(path, sizeof(path), "%s/result.html", dir);
    if (load_file(&files->result, path, "text/html; charset=utf-8") < 0) {
        static_files_free(files);
        return -1;
    }

    /* Load error.html */
    snprintf(path, sizeof(path), "%s/error.html", dir);
    if (load_file(&files->error, path, "text/html; charset=utf-8") < 0) {
        static_files_free(files);
        return -1;
    }

    return 0;
}

void static_files_free(StaticFiles *files)
{
    free_file(&files->broadcast);
    free_file(&files->result);
    free_file(&files->error);
}
