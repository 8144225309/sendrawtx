#include "log.h"

#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>

static LogLevel g_log_level = LOG_INFO;
static char g_identity[32] = "main";
static int g_json_mode = 0;
static int g_verbose = 0;  /* Default: minimal logging, IPs hidden */

static const char *level_names[] = {
    "DEBUG",
    "INFO",
    "WARN",
    "ERROR"
};

static const char *level_colors[] = {
    "\033[36m",  /* DEBUG: cyan */
    "\033[32m",  /* INFO: green */
    "\033[33m",  /* WARN: yellow */
    "\033[31m"   /* ERROR: red */
};

static const char *color_reset = "\033[0m";

void log_init(LogLevel level)
{
    g_log_level = level;
}

void log_set_level(LogLevel level)
{
    g_log_level = level;
}

void log_set_identity(const char *identity)
{
    strncpy(g_identity, identity, sizeof(g_identity) - 1);
    g_identity[sizeof(g_identity) - 1] = '\0';
}

void log_set_json_mode(int json_mode)
{
    g_json_mode = json_mode;
}

/*
 * Escape a string for JSON output.
 * Handles: \n, \r, \t, \", \\
 */
static void json_escape_string(FILE *f, const char *str, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        char c = str[i];
        switch (c) {
            case '"':  fprintf(f, "\\\""); break;
            case '\\': fprintf(f, "\\\\"); break;
            case '\n': fprintf(f, "\\n"); break;
            case '\r': fprintf(f, "\\r"); break;
            case '\t': fprintf(f, "\\t"); break;
            default:
                if ((unsigned char)c < 0x20) {
                    fprintf(f, "\\u%04x", (unsigned char)c);
                } else {
                    fputc(c, f);
                }
        }
    }
}

static void log_write(LogLevel level, const char *fmt, va_list args)
{
    if (level < g_log_level) {
        return;
    }

    /* Get timestamp with microseconds */
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm *tm_info = localtime(&tv.tv_sec);
    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S", tm_info);

    if (g_json_mode) {
        /* JSON format */
        char message[1024];
        vsnprintf(message, sizeof(message), fmt, args);

        fprintf(stderr, "{\"timestamp\":\"%s.%06ldZ\",\"level\":\"%s\",\"worker\":\"%s\",\"message\":\"",
                timestamp, (long)tv.tv_usec, level_names[level], g_identity);
        json_escape_string(stderr, message, strlen(message));
        fprintf(stderr, "\"}\n");
    } else {
        /* Text format */
        char ts_display[24];
        strftime(ts_display, sizeof(ts_display), "%Y-%m-%d %H:%M:%S", tm_info);

        /* Check if stderr is a TTY for colors */
        int use_color = isatty(fileno(stderr));

        /* Print prefix */
        if (use_color) {
            fprintf(stderr, "%s[%s] %s[%-5s]%s [%s] ",
                    color_reset, ts_display,
                    level_colors[level], level_names[level], color_reset,
                    g_identity);
        } else {
            fprintf(stderr, "[%s] [%-5s] [%s] ",
                    ts_display, level_names[level], g_identity);
        }

        /* Print message */
        vfprintf(stderr, fmt, args);
        fprintf(stderr, "\n");
    }

    /* Only flush immediately for errors - reduces I/O blocking under load */
    if (level >= LOG_ERROR) {
        fflush(stderr);
    }
}

void log_debug(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    log_write(LOG_DEBUG, fmt, args);
    va_end(args);
}

void log_info(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    log_write(LOG_INFO, fmt, args);
    va_end(args);
}

void log_warn(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    log_write(LOG_WARN, fmt, args);
    va_end(args);
}

void log_error(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    log_write(LOG_ERROR, fmt, args);
    va_end(args);
}

void log_set_verbose(int verbose)
{
    g_verbose = verbose;
    /* Verbose mode enables DEBUG level and access logging */
    if (verbose) {
        g_log_level = LOG_DEBUG;
    } else {
        g_log_level = LOG_INFO;
    }
}

int log_is_verbose(void)
{
    return g_verbose;
}

/*
 * Format an IP address for logging.
 * In verbose mode: returns full IP for debugging.
 * In minimal mode: returns generic placeholder - no IP info logged.
 */
const char *log_format_ip(const char *ip)
{
    if (!ip || !ip[0]) {
        return "client";
    }

    /* Verbose mode: return full IP */
    if (g_verbose) {
        return ip;
    }

    /* Minimal mode: don't log any IP info */
    return "client";
}

void log_access(const char *client_ip, const char *method, const char *path,
                int status, size_t bytes_sent, double duration_ms,
                const char *request_id)
{
    /* Access logging only enabled in verbose mode */
    if (!g_verbose) {
        return;
    }

    /* Get timestamp with microseconds */
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm *tm_info = localtime(&tv.tv_sec);

    if (g_json_mode) {
        /* JSON format */
        char timestamp[32];
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S", tm_info);

        fprintf(stderr, "{\"timestamp\":\"%s.%06ldZ\",\"type\":\"access\","
                "\"client_ip\":\"%s\",\"method\":\"%s\",\"path\":\"",
                timestamp, (long)tv.tv_usec, client_ip, method);
        /* Escape path for JSON */
        json_escape_string(stderr, path, strlen(path));
        fprintf(stderr, "\",\"status\":%d,\"bytes\":%zu,\"duration_ms\":%.3f,"
                "\"request_id\":\"%s\",\"worker\":\"%s\"}\n",
                status, bytes_sent, duration_ms, request_id, g_identity);
    } else {
        /* Combined Log Format style */
        char timestamp[32];
        strftime(timestamp, sizeof(timestamp), "%d/%b/%Y:%H:%M:%S %z", tm_info);

        fprintf(stderr, "%s - - [%s] \"%s %s HTTP/1.1\" %d %zu %.3fms %s\n",
                client_ip, timestamp, method, path, status, bytes_sent,
                duration_ms, request_id);
    }
    /* No fflush for access logs - let kernel buffer for performance */
}
