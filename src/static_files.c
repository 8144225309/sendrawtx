#include "static_files.h"
#include "network.h"
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

/* Banner placeholder in HTML files */
#define BANNER_PLACEHOLDER "<!-- NETWORK_BANNER -->"

/*
 * Generate banner HTML for a given chain.
 * Returns empty string for mainnet/mixed, banner HTML for test networks.
 */
static const char *get_banner_html(BitcoinChain chain)
{
    switch (chain) {
        case CHAIN_TESTNET:
            return "<div class=\"network-banner network-banner-testnet\">"
                   "TESTNET - Coins have no value"
                   "</div>";
        case CHAIN_SIGNET:
            return "<div class=\"network-banner network-banner-signet\">"
                   "SIGNET - Coins have no value"
                   "</div>";
        case CHAIN_REGTEST:
            return "<div class=\"network-banner network-banner-regtest\">"
                   "REGTEST - Local test network"
                   "</div>";
        case CHAIN_MAINNET:
        default:
            return "";  /* No banner for mainnet or mixed mode */
    }
}

/*
 * Replace placeholder in content with banner HTML.
 * Returns newly allocated string, caller must free.
 * Returns NULL on error.
 */
static char *inject_banner(const char *content, size_t content_len,
                           const char *banner_html, size_t *new_len)
{
    size_t banner_len = strlen(banner_html);
    size_t placeholder_len = strlen(BANNER_PLACEHOLDER);

    /* Find placeholder */
    char *placeholder_pos = strstr(content, BANNER_PLACEHOLDER);
    if (!placeholder_pos) {
        /* No placeholder, return copy of original */
        char *copy = malloc(content_len + 1);
        if (!copy) return NULL;
        memcpy(copy, content, content_len);
        copy[content_len] = '\0';
        *new_len = content_len;
        return copy;
    }

    /* Calculate new size */
    size_t new_size = content_len - placeholder_len + banner_len;
    char *result = malloc(new_size + 1);
    if (!result) return NULL;

    /* Copy content with banner replacing placeholder */
    size_t prefix_len = placeholder_pos - content;
    memcpy(result, content, prefix_len);
    memcpy(result + prefix_len, banner_html, banner_len);
    memcpy(result + prefix_len + banner_len,
           placeholder_pos + placeholder_len,
           content_len - prefix_len - placeholder_len);
    result[new_size] = '\0';

    *new_len = new_size;
    return result;
}

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

/*
 * Inject banner into a loaded static file if needed.
 * For non-mainnet chains, replaces placeholder with banner HTML.
 */
static int inject_banner_into_file(StaticFile *file, BitcoinChain chain)
{
    const char *banner_html = get_banner_html(chain);

    /* No banner needed */
    if (banner_html[0] == '\0') {
        return 0;
    }

    size_t new_len;
    char *new_content = inject_banner(file->content, file->length,
                                      banner_html, &new_len);
    if (!new_content) {
        return -1;
    }

    /* Replace content */
    free(file->content);
    file->content = new_content;
    file->length = new_len;

    return 0;
}

int static_files_load(StaticFiles *files, const char *dir, const Config *config)
{
    char path[512];

    memset(files, 0, sizeof(StaticFiles));

    /* Load index.html */
    snprintf(path, sizeof(path), "%s/index.html", dir);
    if (load_file(&files->index, path, "text/html; charset=utf-8") < 0) {
        static_files_free(files);
        return -1;
    }

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

    /* Load docs.html */
    snprintf(path, sizeof(path), "%s/docs.html", dir);
    if (load_file(&files->docs, path, "text/html; charset=utf-8") < 0) {
        static_files_free(files);
        return -1;
    }

    /* Load status.html */
    snprintf(path, sizeof(path), "%s/status.html", dir);
    if (load_file(&files->status, path, "text/html; charset=utf-8") < 0) {
        static_files_free(files);
        return -1;
    }

    /* Load logos.html */
    snprintf(path, sizeof(path), "%s/logos.html", dir);
    if (load_file(&files->logos, path, "text/html; charset=utf-8") < 0) {
        static_files_free(files);
        return -1;
    }

    /* Inject network banner for non-mainnet chains */
    if (config && network_is_test_network(config->chain)) {
        log_info("Injecting %s banner into HTML files",
                 network_chain_to_string(config->chain));

        if (inject_banner_into_file(&files->index, config->chain) < 0 ||
            inject_banner_into_file(&files->broadcast, config->chain) < 0 ||
            inject_banner_into_file(&files->result, config->chain) < 0 ||
            inject_banner_into_file(&files->error, config->chain) < 0 ||
            inject_banner_into_file(&files->docs, config->chain) < 0 ||
            inject_banner_into_file(&files->status, config->chain) < 0 ||
            inject_banner_into_file(&files->logos, config->chain) < 0) {
            log_error("Failed to inject network banner");
            static_files_free(files);
            return -1;
        }
    }

    return 0;
}

void static_files_free(StaticFiles *files)
{
    free_file(&files->index);
    free_file(&files->broadcast);
    free_file(&files->result);
    free_file(&files->error);
    free_file(&files->docs);
    free_file(&files->status);
    free_file(&files->logos);
}
