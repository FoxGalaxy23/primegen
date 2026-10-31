#define _POSIX_C_SOURCE 200809L
#include "nal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define MLB_MKDIR(p) _mkdir(p)
#else
#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#define MLB_MKDIR(p) mkdir((p), 0700)
#endif

static char g_last_headers[16384];
static char g_last_url[NAL_SYNC_URL_MAX];
static int g_last_status;

static void safe_copy(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0) return;
    if (!src) src = "";
    size_t n = strlen(src); if (n >= cap) n = cap - 1;
    memcpy(dst, src, n); dst[n] = 0;
}

void nal_monitor_set_last_response(const char *headers, const char *url, int status) {
    safe_copy(g_last_headers, sizeof g_last_headers, headers);
    safe_copy(g_last_url, sizeof g_last_url, url);
    g_last_status = status;
}

int nal_get_last_response_metadata(char *headers, size_t headers_cap,
                                   char *url, size_t url_cap, int *status) {
    if (headers && headers_cap) safe_copy(headers, headers_cap, g_last_headers);
    if (url && url_cap) safe_copy(url, url_cap, g_last_url);
    if (status) *status = g_last_status;
    return g_last_status != 0 || g_last_headers[0] != 0 || g_last_url[0] != 0;
}

long long nal_monitor_now_ms(void) {
    struct timespec ts;
#ifdef _WIN32
    /* timespec_get is available in the supported C runtime; monotonic precision is not needed for display. */
    timespec_get(&ts, TIME_UTC);
#else
    clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static void append_safe_url(char *out, size_t cap, const char *url) {
    if (!cap) return;
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)(url ? url : ""); *p && o + 1 < cap; ++p) {
        if (*p == '\r' || *p == '\n' || *p == '\t' || *p < 0x20 || *p == 0x7f) out[o++] = ' ';
        else out[o++] = (char)*p;
    }
    out[o] = 0;
}

void nal_monitor_record(const char *method, const char *url, int status,
                        long long duration_ms, long long bytes, const char *kind) {
    (void)MLB_MKDIR("cache");
    char safe_url[8192], safe_method[64], safe_kind[64];
    append_safe_url(safe_url, sizeof safe_url, url);
    append_safe_url(safe_method, sizeof safe_method, method && *method ? method : "GET");
    append_safe_url(safe_kind, sizeof safe_kind, kind && *kind ? kind : "resource");
    char line[9000];
    int n = snprintf(line, sizeof line, "%lld\t%s\t%d\t%lld\t%lld\t%s\t%s\n",
                     (long long)time(NULL), safe_method, status,
                     duration_ms < 0 ? 0 : duration_ms, bytes < 0 ? 0 : bytes,
                     safe_kind, safe_url);
    if (n <= 0) return;
    size_t len = (size_t)n < sizeof line ? (size_t)n : sizeof line - 1;
#ifdef _WIN32
    FILE *f = fopen("cache/network.log", "ab");
    if (f) { (void)fwrite(line, 1, len, f); (void)fclose(f); }
#else
    int fd = open("cache/network.log", O_CREAT | O_WRONLY | O_APPEND | O_CLOEXEC, 0600);
    if (fd < 0) return;
    (void)flock(fd, LOCK_EX);
    size_t off = 0;
    while (off < len) { ssize_t w = write(fd, line + off, len - off); if (w < 0) { if (errno == EINTR) continue; break; } if (!w) break; off += (size_t)w; }
    (void)flock(fd, LOCK_UN); close(fd);
#endif
}

void nal_monitor_clear(void) {
#ifdef _WIN32
    (void)_unlink("cache/network.log");
#else
    (void)unlink("cache/network.log");
#endif
}
