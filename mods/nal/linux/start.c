#include "nal.h"
#include "gal.h"
#include "db.h"
#include "nhttp.h"
#include "ncors.h"
#include "ntls.h"
#include "idna.h"
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>

/* ── Сетевой слой ──
   По умолчанию используется собственный HTTP/1.1-клиент (nhttp.c) и TLS через системный OpenSSL (ntls_openssl.c):
   без запуска curl, с keep-alive, gzip/deflate, куки по RFC 6265 и проверкой сертификатов.
   Старый путь через curl сохранён как запасной: он включается, если TLS-библиотека не найдена (для https)
   либо задана переменная окружения MLBROWSER_NET=curl. */
static int use_native(const char* url) {
    const char* m = getenv("MLBROWSER_NET");
    if (m && !strcmp(m, "curl")) return 0;
    if (!strncasecmp(url, "http://", 7)) return 1;
    if (!strncasecmp(url, "https://", 8)) return ntls_available();
    return 0;
}

/* Куки профиля: БД → Netscape-файл → jar в памяти. После запроса в БД возвращаются только изменённые. */
static NJar* jar_from_db(void) {
    NJar* jar = njar_new(); if (!jar) return NULL;
    mkdir("cache", 0777);
    char tpl[] = "cache/cookies_in_XXXXXX"; int fd = mkstemp(tpl);
    if (fd >= 0) { close(fd); db_export_cookies_to_file(active_profile_id, tpl); njar_load_netscape(jar, tpl); unlink(tpl); }
    return jar;
}
static void jar_to_db(NJar* jar) {
    mkdir("cache", 0777);
    char tpl[] = "cache/cookies_out_XXXXXX"; int fd = mkstemp(tpl);
    if (fd < 0) return;
    close(fd);
    if (njar_save_changed(jar, tpl) > 0) db_import_cookies_from_file(active_profile_id, tpl);
    unlink(tpl);
}

#define NAL_DOWNLOAD_SLOTS 128
typedef struct {
    int used;
    int id;
    pid_t pid;
    char path[4096];
    char metadata_path[256];
    int paused;
} NalDownloadWorker;
static NalDownloadWorker g_download_workers[NAL_DOWNLOAD_SLOTS];

int nal_init(void) {
    if (!getenv("MLBROWSER_NO_CACHE")) nhttp_cache_config("cache/http", (size_t)256 << 20);       /* HTTP-кэш на диске, до 256 МиБ (LRU) */
    return 1;
}

void nal_cache_policy(int policy) { nhttp_cache_policy(policy); }
void nal_cache_clear(void) { nhttp_cache_clear(); }

void nal_close(void) {
    /* Do not leave network workers or their curl descendants orphaned on exit. */
    for (size_t i = 0; i < NAL_DOWNLOAD_SLOTS; ++i) {
        if (g_download_workers[i].used && g_download_workers[i].pid > 0) {
            (void)kill(-g_download_workers[i].pid, SIGKILL);
            (void)kill(g_download_workers[i].pid, SIGKILL);
            while (waitpid(g_download_workers[i].pid, NULL, 0) < 0 && errno == EINTR) {}
            memset(&g_download_workers[i], 0, sizeof(g_download_workers[i]));
        }
    }
    nhttp_pool_clear();
}

static char* curl_http_get(const char* url);
static int curl_download_file(const char* url, const char* dest_path);
static char g_nal_doc_url[8192];


void nal_set_document_url(const char* url) {
    char upgraded[16384]; const char* use = url ? url : "";
    if (*use && websec_hsts_upgrade_url(use, upgraded, sizeof upgraded)) use = upgraded;
    size_t n = strlen(use);
    if (n >= sizeof g_nal_doc_url) g_nal_doc_url[0] = 0;
    else memcpy(g_nal_doc_url, use, n + 1);
    /* Pass the untruncated identity so the security layer can fail closed if it exceeds its limit. */
    websec_set_document_url(use);
}
const char* nal_current_document_url(void) { return g_nal_doc_url; }

int nal_security_allows(const char* url, int resource_kind) {
    if (!url || !*url) return 0;
    if (resource_kind == NAL_RESOURCE_DOCUMENT) return 1;
    return websec_allows(url, resource_kind);
}

static char* nal_http_get_unchecked(const char* url) {
    if (!use_native(url)) return curl_http_get(url); /* curl path records its own response metadata */
    long long started = nal_monitor_now_ms();
    gal_log("INFO", "NAL: fetching URL (native): '%s'", url);
    NJar* jar = jar_from_db();
    int is_document = websec_is_document_url(url);
    NHttpReq rq; memset(&rq, 0, sizeof rq); rq.url = url; rq.jar = jar;
    rq.site_for_cookies = is_document ? NULL : g_nal_doc_url;
    rq.is_top_level_navigation = is_document;
    NHttpResp rs;
    if (!nhttp_perform(&rq, &rs)) {
        gal_log("ERROR", "NAL: request failed: %s (%s)", rs.err, url);
        nal_monitor_set_last_response(rs.headers ? rs.headers : "", url, rs.status);
        nal_monitor_record("GET", url, rs.status, nal_monitor_now_ms() - started, 0, is_document ? "document" : "resource");
        nhttp_resp_free(&rs); njar_free(jar); return NULL;
    }
    if (rs.from_cache) gal_log("INFO", "NAL: %s из кэша: '%s'", rs.from_cache == 1 ? "свежая запись" : "подтверждено (304)", url);
    const char* final_url = rs.final_url[0] ? rs.final_url : url;
    nal_monitor_set_last_response(rs.headers ? rs.headers : "", final_url, rs.status);
    nal_monitor_record("GET", url, rs.status, nal_monitor_now_ms() - started,
                       (long long)rs.body_len, is_document ? "document" : "resource");
    if (is_document) {
        nal_set_document_url(final_url);
        websec_set_response(final_url, rs.headers ? rs.headers : "");
    }
    if (jar) { jar_to_db(jar); njar_free(jar); }
    char* body = (char*)rs.body; rs.body = NULL;               /* ownership transfers to caller */
    nhttp_resp_free(&rs);
    return body;
}

static int nal_download_file_unchecked(const char* url, const char* dest_path) {
    if (!use_native(url)) return curl_download_file(url, dest_path);
    gal_log("INFO", "NAL: downloading file (native): '%s' -> '%s'", url, dest_path);
    FILE* f = fopen(dest_path, "wb"); if (!f) return 0;
    NHttpReq rq; memset(&rq, 0, sizeof rq); rq.url = url; rq.sink = f; rq.timeout_ms = 120000;
    NHttpResp rs; int ok = nhttp_perform(&rq, &rs);
    fclose(f);
    int good = ok && rs.status >= 200 && rs.status < 300;
    if (!good) { gal_log("WARN", "NAL: download failed (%s): %s", url, ok ? "HTTP-статус не 2xx" : rs.err); unlink(dest_path); }
    nhttp_resp_free(&rs);
    return good;
}

char* nal_http_get_kind(const char* url, int resource_kind) {
    if (!url) return NULL;
    char upgraded[4096]; const char* effective = url;
    if (websec_hsts_upgrade_url(url, upgraded, sizeof upgraded)) effective = upgraded;
    int is_doc = websec_is_document_url(effective) && resource_kind == NAL_RESOURCE_OTHER;
    if (!is_doc && !nal_security_allows(effective, resource_kind)) {
        gal_log("WARN", "CSP: blocked resource %s (kind=%d)", effective, resource_kind);
        return NULL;
    }
    return nal_http_get_unchecked(effective);
}
char* nal_http_get(const char* url) { return nal_http_get_kind(url, NAL_RESOURCE_OTHER); }
int nal_download_file_kind(const char* url, const char* dest_path, int resource_kind) {
    if (!url || !dest_path) return 0;
    char upgraded[4096]; const char* effective = url;
    if (websec_hsts_upgrade_url(url, upgraded, sizeof upgraded)) effective = upgraded;
    if (!nal_security_allows(effective, resource_kind)) {
        gal_log("WARN", "CSP: blocked download %s (kind=%d)", effective, resource_kind);
        return 0;
    }
    return nal_download_file_unchecked(effective, dest_path);
}
int nal_download_file(const char* url, const char* dest_path) { return nal_download_file_kind(url, dest_path, NAL_RESOURCE_OTHER); }


/* ── Safe resumable downloads ──────────────────────────────────────────────
   We append only after the final response headers prove that the server honored
   Range and returned the same representation. The headers callback runs before
   nhttp writes any response body to the sink. */
typedef struct {
    const DBDownloadResumeInfo* info;
    long long range_total;
    int headers_valid;
} NALResumeCheck;

static const char* resume_header_value(const char* headers, const char* name) {
    if (!headers || !name) return NULL;
    size_t nl = strlen(name);
    for (const char* p = headers; *p;) {
        const char* e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n && p[n - 1] == '\r') n--;
        if (n > nl && p[nl] == ':' && !strncasecmp(p, name, nl)) {
            const char* v = p + nl + 1;
            while (v < p + n && (*v == ' ' || *v == '\t')) v++;
            return v;
        }
        if (!e) break;
        p = e + 1;
    }
    return NULL;
}

static int resume_header_equals(const char* headers, const char* name, const char* expected) {
    const char* v = resume_header_value(headers, name);
    if (!v || !expected || !*expected) return 0;
    size_t n = strcspn(v, "\r\n");
    return strlen(expected) == n && memcmp(v, expected, n) == 0;
}

static int resume_headers_callback(const char* headers, const char* response_url,
                                  void* user, char* err, size_t errcap) {
    (void)response_url;
    NALResumeCheck* check = (NALResumeCheck*)user;
    if (!headers || !check || !check->info) return 0;
    int status = 0;
    if (sscanf(headers, "HTTP/%*s %d", &status) != 1 || status != 206) {
        if (err && errcap) snprintf(err, errcap, "resume requires HTTP 206 Partial Content");
        return 0;
    }
    const char* cr = resume_header_value(headers, "Content-Range");
    long long first = -1, last = -1, total = -1;
    if (!cr || sscanf(cr, "bytes %lld-%lld/%lld", &first, &last, &total) != 3 ||
        first != check->info->downloaded_size || last < first || total <= last ||
        (check->info->total_size > 0 && total != check->info->total_size)) {
        if (err && errcap) snprintf(err, errcap, "invalid or mismatched Content-Range");
        return 0;
    }
    int validator_ok = 0;
    int strong_etag = check->info->etag[0] && strncmp(check->info->etag, "W/", 2) != 0;
    if (strong_etag)
        validator_ok = resume_header_equals(headers, "ETag", check->info->etag);
    else if (check->info->last_modified[0])
        validator_ok = resume_header_equals(headers, "Last-Modified", check->info->last_modified);
    if (!validator_ok) {
        if (err && errcap) snprintf(err, errcap, "resume validator changed or missing");
        return 0;
    }
    check->range_total = total;
    check->headers_valid = 1;
    return 1;
}

int nal_resume_download(int download_id) {
    DBDownloadResumeInfo info;
    if (!db_download_get_resume_info(download_id, &info)) {
        gal_log("WARN", "download resume rejected: row %d is not eligible", download_id);
        return 0;
    }
    /* Do not silently bypass the validator checks using the legacy curl path. */
    if (!use_native(info.url)) {
        gal_log("WARN", "download resume requires native NAL transport; curl fallback is not safe yet");
        return 0;
    }
    struct stat before;
    if (stat(info.path, &before) != 0 || !S_ISREG(before.st_mode) ||
        (long long)before.st_size != info.downloaded_size) return 0;
    if (info.total_size > 0 && info.downloaded_size == info.total_size) {
        db_update_download_progress(download_id, info.downloaded_size, "complete");
        return 1;
    }
    FILE* f = fopen(info.path, "ab");
    if (!f) return 0;

    char headers[2300];
    if (info.etag[0] && strncmp(info.etag, "W/", 2) != 0)
        snprintf(headers, sizeof(headers), "Range: bytes=%lld-\nIf-Range: %s\nAccept-Encoding: identity", info.downloaded_size, info.etag);
    else
        snprintf(headers, sizeof(headers), "Range: bytes=%lld-\nIf-Range: %s\nAccept-Encoding: identity", info.downloaded_size, info.last_modified);
    NALResumeCheck check; memset(&check, 0, sizeof(check)); check.info = &info;
    NHttpReq rq; memset(&rq, 0, sizeof(rq));
    rq.url = info.url; rq.headers = headers; rq.sink = f; rq.timeout_ms = 120000;
    rq.max_redirects = 10; rq.headers_cb = resume_headers_callback; rq.headers_user = &check;
    NHttpResp rs; memset(&rs, 0, sizeof(rs));
    int transport_ok = nhttp_perform(&rq, &rs);
    int close_ok = fclose(f) == 0;
    struct stat after; long long actual = -1;
    if (stat(info.path, &after) == 0 && S_ISREG(after.st_mode)) actual = (long long)after.st_size;
    int success = transport_ok && close_ok && check.headers_valid && rs.status == 206 &&
                  actual == check.range_total && actual >= info.downloaded_size;
    if (success) {
        db_update_download_progress(download_id, actual, "complete");
        db_download_set_validators(download_id, info.etag, info.last_modified, 1);
        gal_log("INFO", "download %d resumed successfully (%lld bytes)", download_id, actual);
    } else {
        /* If the callback rejected headers, no response bytes should have reached the file.
           If a valid partial body failed mid-transfer, retain it as an interrupted partial. */
        if (!check.headers_valid && actual >= 0 && actual != info.downloaded_size) {
            if (truncate(info.path, (off_t)info.downloaded_size) == 0) actual = info.downloaded_size;
        }
        if (actual < 0) actual = info.downloaded_size;
        db_update_download_progress(download_id, actual, "interrupted");
        gal_log("WARN", "download %d resume failed (status=%d, bytes=%lld, error=%s)",
                download_id, rs.status, actual, rs.err[0] ? rs.err : "invalid partial response");
    }
    nhttp_resp_free(&rs);
    return success;
}

/* Child-only resume transfer. Database work stays in the parent process. */
static int download_resume_transfer(const DBDownloadResumeInfo* info) {
    if (!info || !info->id || !use_native(info->url)) return 0;
    struct stat before;
    if (stat(info->path, &before) != 0 || !S_ISREG(before.st_mode) ||
        (long long)before.st_size != info->downloaded_size) return 0;
    if (info->total_size > 0 && info->downloaded_size == info->total_size) return 1;
    FILE* f = fopen(info->path, "ab");
    if (!f) return 0;
    char headers[2300];
    const char* validator = (info->etag[0] && strncmp(info->etag, "W/", 2) != 0) ? info->etag : info->last_modified;
    if (!validator || !*validator) { fclose(f); return 0; }
    int hn = snprintf(headers, sizeof(headers), "Range: bytes=%lld-\nIf-Range: %s\nAccept-Encoding: identity",
                      info->downloaded_size, validator);
    if (hn < 0 || (size_t)hn >= sizeof(headers)) { fclose(f); return 0; }
    NALResumeCheck check; memset(&check, 0, sizeof(check)); check.info = info;
    NHttpReq rq; memset(&rq, 0, sizeof(rq));
    rq.url = info->url; rq.headers = headers; rq.sink = f; rq.timeout_ms = 120000;
    rq.max_redirects = 10; rq.headers_cb = resume_headers_callback; rq.headers_user = &check;
    NHttpResp rs; memset(&rs, 0, sizeof(rs));
    int transport_ok = nhttp_perform(&rq, &rs);
    int close_ok = fclose(f) == 0;
    struct stat after; long long actual = -1;
    if (stat(info->path, &after) == 0 && S_ISREG(after.st_mode)) actual = (long long)after.st_size;
    int success = transport_ok && close_ok && check.headers_valid && rs.status == 206 &&
                  actual == check.range_total && actual >= info->downloaded_size;
    if (!success && !check.headers_valid && actual >= 0 && actual != info->downloaded_size) {
        if (truncate(info->path, (off_t)info->downloaded_size) == 0) actual = info->downloaded_size;
    }
    if (!success) gal_log("WARN", "background resume failed id=%d status=%d bytes=%lld error=%s",
                          info->id, rs.status, actual, rs.err[0] ? rs.err : "invalid partial response");
    nhttp_resp_free(&rs);
    return success;
}

/* ── Background download process controller ────────────────────────────────
   The child performs blocking network I/O but never touches the inherited SQLite handle.
   Response metadata is written atomically to a sidecar; the parent imports it into SQLite. */

static NalDownloadWorker* download_worker_by_id(int id) {
    for (size_t i = 0; i < NAL_DOWNLOAD_SLOTS; ++i)
        if (g_download_workers[i].used && g_download_workers[i].id == id) return &g_download_workers[i];
    return NULL;
}


int nal_download_resume_start(int download_id) {
    DBDownloadResumeInfo info;
    if (!db_download_get_resume_info(download_id, &info) || download_worker_by_id(download_id) || !use_native(info.url)) return -1;
    NalDownloadWorker* slot = NULL;
    for (size_t i = 0; i < NAL_DOWNLOAD_SLOTS; ++i) if (!g_download_workers[i].used) { slot = &g_download_workers[i]; break; }
    if (!slot) return -1;
    if (info.total_size > 0 && info.downloaded_size == info.total_size) {
        db_update_download_progress(download_id, info.downloaded_size, "complete");
        return download_id;
    }
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) { (void)setpgid(0, 0); _exit(download_resume_transfer(&info) ? 0 : 1); }
    (void)setpgid(pid, pid);
    memset(slot, 0, sizeof(*slot)); slot->used = 1; slot->id = download_id; slot->pid = pid;
    snprintf(slot->path, sizeof(slot->path), "%s", info.path);
    db_update_download_progress(download_id, info.downloaded_size, "downloading");
    return download_id;
}

static int copy_header_value(const char* headers, const char* name, char* out, size_t cap) {
    if (!out || cap < 2) return 0;
    out[0] = 0;
    const char* v = resume_header_value(headers, name);
    if (!v) return 0;
    size_t n = strcspn(v, "\r\n");
    while (n && (v[n - 1] == ' ' || v[n - 1] == '\t')) --n;
    if (n >= cap) return 0;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)v[i];
        if (c < 0x20 || c == 0x7f) return 0;
    }
    memcpy(out, v, n); out[n] = 0;
    return 1;
}

static int write_download_metadata(const char* path, long long total, const char* etag,
                                   const char* modified, int ranges) {
    if (!path || !*path) return 0;
    char tmp[320];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) return 0;
    FILE* f = fopen(tmp, "wb");
    if (!f) return 0;
    int ok = fprintf(f, "%lld\n%d\n%s\n%s\n", total, ranges ? 1 : 0,
                     etag ? etag : "", modified ? modified : "") > 0;
    if (ok) ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (fclose(f) != 0) ok = 0;
    if (ok) ok = rename(tmp, path) == 0;
    if (!ok) unlink(tmp);
    return ok;
}

typedef struct { const char* metadata_path; long long total_size; int headers_seen; } DownloadResponseMeta;

static int download_response_headers_callback(const char* headers, const char* response_url,
                                               void* user, char* err, size_t errcap) {
    (void)response_url;
    DownloadResponseMeta* meta = (DownloadResponseMeta*)user;
    int status = 0;
    if (!headers || !meta || !meta->metadata_path ||
        sscanf(headers, "HTTP/%*s %d", &status) != 1 || status < 200 || status >= 300) {
        if (err && errcap) snprintf(err, errcap, "download requires a successful 2xx response");
        return 0;
    }
    char etag[1024] = "", modified[256] = "", length_text[64] = "", ranges_text[64] = "";
    (void)copy_header_value(headers, "ETag", etag, sizeof(etag));
    (void)copy_header_value(headers, "Last-Modified", modified, sizeof(modified));
    (void)copy_header_value(headers, "Content-Length", length_text, sizeof(length_text));
    (void)copy_header_value(headers, "Accept-Ranges", ranges_text, sizeof(ranges_text));
    long long total = -1;
    if (length_text[0]) {
        char* end = NULL; errno = 0;
        long long parsed = strtoll(length_text, &end, 10);
        if (!errno && end && *end == 0 && parsed >= 0) total = parsed;
    }
    int byte_ranges = !strncasecmp(ranges_text, "bytes", 5) &&
        (ranges_text[5] == 0 || ranges_text[5] == ' ' || ranges_text[5] == '\t');
    int stable_validator = (etag[0] && strncmp(etag, "W/", 2) != 0) || modified[0];
    if (!write_download_metadata(meta->metadata_path, total, etag, modified,
                                 byte_ranges && stable_validator)) {
        if (err && errcap) snprintf(err, errcap, "could not persist response metadata");
        return 0;
    }
    meta->total_size = total;
    meta->headers_seen = 1;
    return 1;
}

static int download_worker_transfer(const char* url, const char* dest_path, const char* metadata_path) {
    /* The resume contract is implemented only in NHttp, where final headers can be validated
       before any body bytes are committed. Do not silently use curl for a resumable record. */
    if (!use_native(url)) {
        gal_log("WARN", "background resumable download requires native NAL transport");
        return 0;
    }
    DownloadResponseMeta meta; memset(&meta, 0, sizeof(meta));
    meta.metadata_path = metadata_path; meta.total_size = -1;
    FILE* f = fopen(dest_path, "wb");
    if (!f) return 0;
    NHttpReq rq; memset(&rq, 0, sizeof(rq));
    rq.url = url; rq.sink = f; rq.timeout_ms = 120000; rq.max_redirects = 10;
    rq.headers_cb = download_response_headers_callback; rq.headers_user = &meta;
    NHttpResp rs; memset(&rs, 0, sizeof(rs));
    int transport_ok = nhttp_perform(&rq, &rs);
    int close_ok = fclose(f) == 0;
    struct stat sb;
    int regular = stat(dest_path, &sb) == 0 && S_ISREG(sb.st_mode) && sb.st_size >= 0;
    int expected_size_ok = regular && (meta.total_size < 0 || (long long)sb.st_size == meta.total_size);
    int good = transport_ok && close_ok && meta.headers_seen && rs.status >= 200 && rs.status < 300 && expected_size_ok;
    if (!good) {
        /* Keep actual partial bytes for an interruption; only successful completion becomes complete. */
        gal_log("WARN", "background download failed (%s): status=%d error=%s",
                url, rs.status, rs.err[0] ? rs.err : "write/network error");
    }
    nhttp_resp_free(&rs);
    return good;
}

static int download_apply_metadata(NalDownloadWorker* w) {
    if (!w || !w->metadata_path[0]) return 0;
    FILE* f = fopen(w->metadata_path, "rb");
    if (!f) return 0;
    char total_line[64], range_line[16], etag[1024], modified[256];
    int ok = fgets(total_line, sizeof(total_line), f) && fgets(range_line, sizeof(range_line), f) &&
             fgets(etag, sizeof(etag), f) && fgets(modified, sizeof(modified), f);
    fclose(f);
    if (!ok) return 0;
    total_line[strcspn(total_line, "\r\n")] = 0;
    range_line[strcspn(range_line, "\r\n")] = 0;
    etag[strcspn(etag, "\r\n")] = 0;
    modified[strcspn(modified, "\r\n")] = 0;
    char* end = NULL; errno = 0; long long total = strtoll(total_line, &end, 10);
    if (errno || !end || *end || total < -1 || (strcmp(range_line, "0") && strcmp(range_line, "1"))) return 0;
    /* Protect the line-based sidecar parser from malformed or accidentally replaced files. */
    if (strchr(etag, '\t') || strchr(modified, '\t')) return 0;
    int result = db_download_update_response(w->id, total, etag, modified, atoi(range_line) == 1);
    if (result) unlink(w->metadata_path);
    return result;
}

int nal_download_start(const char* url, const char* dest_path, const char* filename) {
    if (!url || !*url || !dest_path || !*dest_path || !filename || !*filename) return -1;
    size_t ul = strlen(url), pl = strlen(dest_path), fl = strlen(filename);
    if (ul >= NHTTP_URL_MAX || pl >= sizeof(g_download_workers[0].path) || fl > 1024) return -1;
    char upgraded[NHTTP_URL_MAX];
    const char* effective = url;
    if (websec_hsts_upgrade_url(url, upgraded, sizeof(upgraded))) effective = upgraded;
    if ((strncasecmp(effective, "http://", 7) && strncasecmp(effective, "https://", 8)) || !use_native(effective)) return -1;
    url = effective;
    NalDownloadWorker* slot = NULL;
    for (size_t i = 0; i < NAL_DOWNLOAD_SLOTS; ++i) if (!g_download_workers[i].used) { slot = &g_download_workers[i]; break; }
    if (!slot) return -1;
    if (mkdir("cache", 0700) != 0 && errno != EEXIST) return -1;
    int id = db_add_download(url, dest_path, filename, -1);
    if (id <= 0) return -1;
    char metadata_path[256];
    if (snprintf(metadata_path, sizeof(metadata_path), "cache/download_%d.meta", id) >= (int)sizeof(metadata_path)) {
        db_update_download_progress(id, 0, "failed"); return -1;
    }
    unlink(metadata_path);
    db_update_download_progress(id, 0, "starting");
    pid_t pid = fork();
    if (pid < 0) { db_update_download_progress(id, 0, "failed"); return -1; }
    if (pid == 0) {
        (void)setpgid(0, 0);
        int ok = download_worker_transfer(url, dest_path, metadata_path);
        _exit(ok ? 0 : 1);
    }
    (void)setpgid(pid, pid);
    memset(slot, 0, sizeof(*slot)); slot->used = 1; slot->id = id; slot->pid = pid;
    memcpy(slot->path, dest_path, pl + 1); snprintf(slot->metadata_path, sizeof(slot->metadata_path), "%s", metadata_path);
    db_update_download_progress(id, 0, "downloading");
    return id;
}

int nal_download_pause(int id) {
    NalDownloadWorker* w = download_worker_by_id(id);
    if (!w || w->paused || kill(-w->pid, SIGSTOP) != 0) return 0;
    w->paused = 1;
    (void)download_apply_metadata(w);
    struct stat st; long long bytes = (stat(w->path, &st) == 0 && S_ISREG(st.st_mode)) ? (long long)st.st_size : 0;
    db_update_download_progress(id, bytes, "paused");
    return 1;
}

int nal_download_continue(int id) {
    NalDownloadWorker* w = download_worker_by_id(id);
    if (!w || !w->paused || kill(-w->pid, SIGCONT) != 0) return 0;
    w->paused = 0;
    struct stat st; long long bytes = (stat(w->path, &st) == 0 && S_ISREG(st.st_mode)) ? (long long)st.st_size : 0;
    db_update_download_progress(id, bytes, "downloading");
    return 1;
}

int nal_download_cancel(int id) {
    NalDownloadWorker* w = download_worker_by_id(id);
    if (!w) return 0;
    if (kill(-w->pid, SIGKILL) != 0 && errno != ESRCH) return 0;
    int status = 0; while (waitpid(w->pid, &status, 0) < 0 && errno == EINTR) {}
    (void)download_apply_metadata(w);
    struct stat st; long long bytes = (stat(w->path, &st) == 0 && S_ISREG(st.st_mode)) ? (long long)st.st_size : 0;
    db_update_download_progress(id, bytes, bytes > 0 ? "interrupted" : "failed");
    memset(w, 0, sizeof(*w));
    return 1;
}

void nal_download_poll(void) {
    for (size_t i = 0; i < NAL_DOWNLOAD_SLOTS; ++i) {
        NalDownloadWorker* w = &g_download_workers[i];
        if (!w->used) continue;
        (void)download_apply_metadata(w);
        struct stat st; long long bytes = (stat(w->path, &st) == 0 && S_ISREG(st.st_mode)) ? (long long)st.st_size : 0;
        if (w->paused) { db_update_download_progress(w->id, bytes, "paused"); continue; }
        int status = 0; pid_t r = waitpid(w->pid, &status, WNOHANG);
        if (r == 0) { db_update_download_progress(w->id, bytes, "downloading"); continue; }
        if (r < 0 && errno != ECHILD) db_update_download_progress(w->id, bytes, bytes ? "interrupted" : "failed");
        else if (r == w->pid && WIFEXITED(status) && WEXITSTATUS(status) == 0)
            db_update_download_progress(w->id, bytes, "complete");
        else db_update_download_progress(w->id, bytes, bytes ? "interrupted" : "failed");
        if (w->metadata_path[0]) unlink(w->metadata_path);
        memset(w, 0, sizeof(*w));
    }
}

static char* read_text_file(const char* path, size_t limit) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long size = ftell(f);
    if (size < 0 || (size_t)size > limit || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    char* data = malloc((size_t)size + 1);
    if (!data) { fclose(f); return NULL; }
    size_t got = fread(data, 1, (size_t)size, f);
    fclose(f); data[got] = 0;
    return data;
}

/* curl fallback never uses a shell and deliberately sends/imports no cookies: the external
   cookie engine cannot currently enforce this browser's schemeful SameSite policy. */
static int curl_exec_to_files(const char* url, const char* body_path, const char* header_path,
                              const char* effective_path, int require_2xx) {
    pid_t pid = fork();
    if (pid < 0) return 0;
    if (pid == 0) {
        int outfd = open(effective_path ? effective_path : "/dev/null", O_WRONLY | O_TRUNC);
        if (outfd < 0 || dup2(outfd, STDOUT_FILENO) < 0) _exit(126);
        if (outfd != STDOUT_FILENO) close(outfd);
        char* argv[32]; int n = 0;
        argv[n++] = (char*)"curl";
        argv[n++] = (char*)"--silent"; argv[n++] = (char*)"--show-error";
        argv[n++] = (char*)"--location"; argv[n++] = (char*)"--max-redirs"; argv[n++] = (char*)"10";
        argv[n++] = (char*)"--max-time"; argv[n++] = (char*)"60";
        argv[n++] = (char*)"--proto"; argv[n++] = (char*)"=http,https";
        argv[n++] = (char*)"--proto-redir"; argv[n++] = (char*)"=http,https";
        argv[n++] = (char*)"--user-agent"; argv[n++] = (char*)"Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 Chrome/120 Safari/537.36";
        if (require_2xx) argv[n++] = (char*)"--fail";
        if (header_path) { argv[n++] = (char*)"--dump-header"; argv[n++] = (char*)header_path; }
        argv[n++] = (char*)"--output"; argv[n++] = (char*)body_path;
        if (effective_path) { argv[n++] = (char*)"--write-out"; argv[n++] = (char*)"%{url_effective}"; }
        argv[n++] = (char*)"--url"; argv[n++] = (char*)url; argv[n] = NULL;
        execvp("curl", argv); _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) { if (errno != EINTR) return 0; }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static char* final_header_block(char* headers) {
    if (!headers) return NULL;
    char* last = NULL;
    for (char* p = headers; *p;) {
        if (!strncasecmp(p, "HTTP/", 5)) last = p;
        char* e = strchr(p, '\n'); if (!e) break; p = e + 1;
    }
    return last ? last : headers;
}

static void remember_hsts_from_url(const char* url, const char* headers) {
    if (!url || strncasecmp(url, "https://", 8) || !headers) return;
    const char* a = url + 8; const char* end = a + strcspn(a, "/?#");
    const char* at = NULL; for (const char* p = a; p < end; p++) if (*p == '@') at = p;
    if (at) a = at + 1;
    char host[256], ascii[256]; const char* he = end;
    if (a < end && *a == '[') return; /* HSTS does not apply to IP literals. */
    for (const char* p = a; p < end; p++) if (*p == ':') { he = p; break; }
    size_t n = (size_t)(he - a); if (!n || n >= sizeof host) return;
    memcpy(host, a, n); host[n] = 0;
    if (!nidna_to_ascii(host, ascii, sizeof ascii)) return;
    websec_hsts_remember(ascii, 1, headers);
}

static char* curl_http_get(const char* url) {
    long long started = nal_monitor_now_ms();
    int is_document = websec_is_document_url(url);
    gal_log("INFO", "NAL: fetching URL (curl, no cookies): '%s'", url);
    mkdir("cache", 0700); mkdir("cache/security", 0700);
    char body[] = "cache/curl_body_XXXXXX", headers_path[] = "cache/curl_headers_XXXXXX", effective_path[] = "cache/curl_url_XXXXXX";
    int bfd = mkstemp(body), hfd = mkstemp(headers_path), efd = mkstemp(effective_path);
    if (bfd < 0 || hfd < 0 || efd < 0) {
        if (bfd >= 0) { close(bfd); unlink(body); } if (hfd >= 0) { close(hfd); unlink(headers_path); }
        if (efd >= 0) { close(efd); unlink(effective_path); } return NULL;
    }
    close(bfd); close(hfd); close(efd);
    int ok = curl_exec_to_files(url, body, headers_path, effective_path, 0);
    char* effective = ok ? read_text_file(effective_path, 8192) : NULL;
    char* raw_headers = ok ? read_text_file(headers_path, 1024 * 1024) : NULL;
    char* header_block = raw_headers ? final_header_block(raw_headers) : NULL;
    int status = 0;
    if (header_block) (void)sscanf(header_block, "HTTP/%*s %d", &status);
    if (ok && effective && *effective && header_block) {
        remember_hsts_from_url(effective, header_block);
        nal_monitor_set_last_response(header_block, effective, status);
        if (is_document) {
            nal_set_document_url(effective);
            websec_set_response(effective, header_block);
        }
    } else {
        nal_monitor_set_last_response(header_block ? header_block : "", effective ? effective : url, status);
    }
    char* buffer = ok ? read_text_file(body, 64u * 1024u * 1024u) : NULL;
    nal_monitor_record("GET", url, status, nal_monitor_now_ms() - started,
                       buffer ? (long long)strlen(buffer) : 0, is_document ? "document" : "resource");
    free(effective); free(raw_headers);
    unlink(body); unlink(headers_path); unlink(effective_path);
    if (!ok) gal_log("ERROR", "NAL: curl fallback failed (exit/network/protocol error)");
    return buffer;
}

static int curl_download_file(const char* url, const char* dest_path) {
    gal_log("INFO", "NAL: downloading file (curl, no cookies): '%s' -> '%s'", url, dest_path);
    int ok = curl_exec_to_files(url, dest_path, NULL, NULL, 1);
    if (!ok) unlink(dest_path);
    return ok;
}

/* ── Асинхронные запросы: fork() + execvp("curl", ...) напрямую (без shell — argv строится вручную,
   так что тело/заголовки/URL не нуждаются в экранировании и не текут через /bin/sh -c). Родитель
   тут же возвращается; готовность потом проверяется неблокирующим waitpid(WNOHANG) из nal_async_poll(). */
static void split_headers(const char* raw, char** argv, int* argc, int max) {
    if (!raw) return;
    char* dup = strdup(raw); char* save = dup;
    char* line;
    while (*argc + 2 < max && (line = strsep(&dup, "\n"))) {
        if (!*line) continue;
        argv[(*argc)++] = strdup("-H");
        argv[(*argc)++] = strdup(line);
    }
    free(save);
}

/* Заголовки публикуются атомарно: родительский процесс увидит либо пустой файл, либо целый финальный блок. */
static int stream_headers_to_file(const char* headers, const char* response_url, void* user, char* err, size_t errcap) {
    const char* path = (const char*)user;
    if (!path || !*path) { if (err && errcap) snprintf(err, errcap, "нет пути файла заголовков"); return 0; }
    char tmp[192]; snprintf(tmp, sizeof tmp, "%s.ready", path);
    FILE* f = fopen(tmp, "wb");
    if (!f) { if (err && errcap) snprintf(err, errcap, "не удалось создать файл заголовков потока"); return 0; }
    size_t hlen = headers ? strlen(headers) : 0;
    /* The transport callback includes the HTTP header terminator. Insert private metadata
       before that terminator, otherwise it becomes a second header block and the JS parser
       can accidentally select that block instead of the actual response headers. */
    size_t block_len = hlen;
    if (hlen >= 4 && !memcmp(headers + hlen - 4, "\r\n\r\n", 4)) block_len = hlen - 2;
    else if (hlen >= 2 && !memcmp(headers + hlen - 2, "\n\n", 2)) block_len = hlen - 1;
    int ok = (!block_len || fwrite(headers, 1, block_len, f) == block_len);
    if (ok && block_len && headers[block_len - 1] != '\n') ok = fwrite("\r\n", 1, 2, f) == 2;
    /* Internal metadata is stripped before headers are exposed to JS/XHR. */
    if (ok) ok = fprintf(f, "X-MLBrowser-Internal-URL: %s\r\n\r\n", response_url ? response_url : "") > 0;
    if (ok) ok = fflush(f) == 0;
    if (ok) ok = fsync(fileno(f)) == 0;
    if (fclose(f) != 0) ok = 0;
    if (ok) ok = rename(tmp, path) == 0;
    if (!ok) { unlink(tmp); if (err && errcap) snprintf(err, errcap, "ошибка публикации заголовков потока"); }
    return ok;
}

/* Тело потомка для асинхронного запроса. При streaming сетевой read loop пишет в bounded pipe; если JS не читает —
   ядро останавливает писателя и TCP получает backpressure. Заголовки публикуются до чтения тела. */
static int native_async_child(const char* method, const char* url, const char* headers_raw, const char* req_path,
                              const char* cin, const char* cout, const char* hdr_path, const char* body_path,
                              const NalFetchOpts* opts, int streaming, int stream_fd) {
    unsigned char* body = NULL; size_t blen = 0;
    if (req_path && req_path[0]) {
        FILE* rf = fopen(req_path, "rb");
        if (rf) { fseek(rf, 0, SEEK_END); long sz = ftell(rf); fseek(rf, 0, SEEK_SET); if (sz > 0) { body = malloc((size_t)sz); if (body) blen = fread(body, 1, (size_t)sz, rf); } fclose(rf); }
    }
    NJar* jar = njar_new(); if (jar && cin) njar_load_netscape(jar, cin);
    FILE* bf = NULL;
    if (streaming) {
        bf = fdopen(stream_fd, "wb");
        if (!bf) { close(stream_fd); njar_free(jar); free(body); return 1; }
        setvbuf(bf, NULL, _IONBF, 0);
    }
    NCorsReq cr; memset(&cr, 0, sizeof cr); cr.method = method; cr.url = url; cr.headers = headers_raw; cr.body = body; cr.body_len = blen; cr.jar = jar; cr.timeout_ms = 60000;
    if (streaming) { cr.sink = bf; cr.headers_cb = stream_headers_to_file; cr.headers_user = (void*)hdr_path; }
    if (opts && opts->page_url) { cr.page_url = opts->page_url; cr.credentials = opts->credentials; cr.mode = opts->mode; cr.redirect = opts->redirect; }
    NHttpResp rs; int ok = ncors_fetch(&cr, &rs);
    if (bf) { fflush(bf); fclose(bf); bf = NULL; }
    if (!ok && opts && opts->page_url) gal_log("WARN", "fetch заблокирован/не удался (%s → %s): %s", opts->page_url, url, rs.err);
    if (ok) {
        if (!streaming) {
            FILE* hf = fopen(hdr_path, "wb"); if (hf) { if (rs.headers) fputs(rs.headers, hf); fputs("\r\n", hf); fclose(hf); }
            FILE* whole = fopen(body_path, "wb"); if (whole) { if (rs.body_len) fwrite(rs.body, 1, rs.body_len, whole); fclose(whole); }
        }
        if (jar && cout && cr.jar_used) njar_save_changed(jar, cout);
    }
    nhttp_resp_free(&rs); njar_free(jar); free(body);
    return ok ? 0 : 1;
}

void nal_sync_response_free(NalSyncResponse* response) {
    if (!response) return;
    free(response->headers); free(response->body); memset(response, 0, sizeof *response);
}

int nal_sync_fetch_ex(const char* method, const char* url, const char* headers_raw,
                      const unsigned char* body, size_t body_len, const NalFetchOpts* opts,
                      NalSyncResponse* out) {
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    if (!url || !*url) { snprintf(out->error, sizeof out->error, "empty URL"); return 0; }
    NJar* jar = jar_from_db();
    NCorsReq cr; memset(&cr, 0, sizeof cr);
    cr.method = method && *method ? method : "GET"; cr.url = url; cr.headers = headers_raw;
    cr.body = body; cr.body_len = body_len; cr.jar = jar; cr.timeout_ms = 60000;
    if (opts) { cr.page_url = opts->page_url; cr.credentials = opts->credentials; cr.mode = opts->mode; cr.redirect = opts->redirect; }
    NHttpResp rs; memset(&rs, 0, sizeof rs);
    int ok = ncors_fetch(&cr, &rs);
    if (jar && cr.jar_used) jar_to_db(jar);
    if (ok) {
        out->status = rs.status;
        out->headers = rs.headers ? strdup(rs.headers) : strdup("");
        out->body = rs.body; out->body_len = rs.body_len; rs.body = NULL;
        snprintf(out->final_url, sizeof out->final_url, "%s", rs.final_url);
        if (!out->headers || (!out->body && out->body_len)) {
            snprintf(out->error, sizeof out->error, "out of memory"); ok = 0;
        }
    } else {
        snprintf(out->error, sizeof out->error, "%s", rs.err[0] ? rs.err : "synchronous request failed");
    }
    nhttp_resp_free(&rs); njar_free(jar);
    if (!ok) {
        char saved_error[sizeof out->error]; snprintf(saved_error, sizeof saved_error, "%s", out->error);
        nal_sync_response_free(out); out->status = 0; snprintf(out->error, sizeof out->error, "%s", saved_error[0] ? saved_error : "synchronous request failed");
    }
    return ok;
}

int nal_async_start(const char* method, const char* url, const char* headers_raw, const char* body, size_t body_len, NalAsyncReq* out) {
    return nal_async_start_ex(method, url, headers_raw, body, body_len, NULL, out);
}

static int nal_async_start_impl(const char* method, const char* url, const char* headers_raw, const char* body, size_t body_len, const NalFetchOpts* opts, int streaming, NalAsyncReq* out) {
    memset(out, 0, sizeof *out);
    if (streaming && !use_native(url)) return 0;                            /* fallback curl не умеет безопасно публиковать финальные заголовки и поток тела */
    if (opts && opts->page_url && !use_native(url)) return 0;               /* политику CORS обеспечивает только собственный клиент */
    mkdir("cache", 0777);
    char body_tpl[] = "cache/areq_b_XXXXXX", hdr_tpl[] = "cache/areq_h_XXXXXX", cin_tpl[] = "cache/areq_ci_XXXXXX", cout_tpl[] = "cache/areq_co_XXXXXX", req_tpl[] = "cache/areq_rq_XXXXXX";
    int body_pipe[2] = {-1,-1};
    int bf = streaming ? -1 : mkstemp(body_tpl); if (streaming) body_tpl[0] = 0;
    int hf = mkstemp(hdr_tpl); int cif = mkstemp(cin_tpl); int cof = mkstemp(cout_tpl);
    if (bf < 0 && !streaming) { if (hf >= 0) { close(hf); unlink(hdr_tpl); } if (cif >= 0) { close(cif); unlink(cin_tpl); } if (cof >= 0) { close(cof); unlink(cout_tpl); } return 0; }
    if (hf < 0 || cif < 0 || cof < 0) { if (bf >= 0) { close(bf); unlink(body_tpl); } if (hf >= 0) { close(hf); unlink(hdr_tpl); } if (cif >= 0) { close(cif); unlink(cin_tpl); } if (cof >= 0) { close(cof); unlink(cout_tpl); } return 0; }
    close(hf); close(cof); if (bf >= 0) close(bf);
    db_export_cookies_to_file(active_profile_id, cin_tpl); close(cif);
    if (streaming && pipe(body_pipe) != 0) { unlink(hdr_tpl); unlink(cin_tpl); unlink(cout_tpl); return 0; }

    int req_fd = -1;
    if (body && body_len) {
        req_fd = mkstemp(req_tpl);
        if (req_fd < 0) {
            if (body_pipe[0] >= 0) close(body_pipe[0]); if (body_pipe[1] >= 0) close(body_pipe[1]);
            if (body_tpl[0]) unlink(body_tpl); unlink(hdr_tpl); unlink(cin_tpl); unlink(cout_tpl); return 0;
        }
        size_t off = 0; int write_failed = 0;
        while (off < body_len) {
            ssize_t w = write(req_fd, body + off, body_len - off);
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) { write_failed = 1; break; }
            off += (size_t)w;
        }
        close(req_fd);
        if (write_failed) {
            if (body_pipe[0] >= 0) close(body_pipe[0]); if (body_pipe[1] >= 0) close(body_pipe[1]);
            if (body_tpl[0]) unlink(body_tpl); unlink(hdr_tpl); unlink(cin_tpl); unlink(cout_tpl); unlink(req_tpl); return 0;
        }
    }

    /* Create the cleanup manifest before fork so even startup failures cannot strand temp files. */
    char side[160]; snprintf(side, sizeof side, "%s.aux", hdr_tpl);
    FILE* sf = fopen(side, "w");
    if (!sf) {
        if (body_pipe[0] >= 0) close(body_pipe[0]); if (body_pipe[1] >= 0) close(body_pipe[1]);
        if (body_tpl[0]) unlink(body_tpl); unlink(hdr_tpl); unlink(cin_tpl); unlink(cout_tpl); if (req_fd >= 0) unlink(req_tpl); return 0;
    }
    int manifest_ok = fprintf(sf, "%s\n%s\n%s", cin_tpl, cout_tpl, req_fd >= 0 ? req_tpl : "") > 0;
    if (fclose(sf) != 0) manifest_ok = 0;
    if (!manifest_ok) {
        if (body_pipe[0] >= 0) close(body_pipe[0]); if (body_pipe[1] >= 0) close(body_pipe[1]);
        if (body_tpl[0]) unlink(body_tpl); unlink(hdr_tpl); unlink(side); unlink(cin_tpl); unlink(cout_tpl); if (req_fd >= 0) unlink(req_tpl); return 0;
    }

    pid_t pid = fork();
    if (pid < 0) { if (body_pipe[0] >= 0) close(body_pipe[0]); if (body_pipe[1] >= 0) close(body_pipe[1]); if (body_tpl[0]) unlink(body_tpl); unlink(hdr_tpl); unlink(side); unlink(cin_tpl); unlink(cout_tpl); if (req_fd >= 0) unlink(req_tpl); return 0; }
    if (pid == 0) {
        if (streaming) close(body_pipe[0]);
        int devnull = open("/dev/null", O_WRONLY); if (devnull >= 0) { dup2(devnull, 1); dup2(devnull, 2); }
        if (use_native(url)) _exit(native_async_child(method, url, headers_raw, req_fd >= 0 ? req_tpl : NULL, cin_tpl, cout_tpl, hdr_tpl, body_tpl, opts, streaming, streaming ? body_pipe[1] : -1));
        if (streaming) _exit(127);
        char* argv[64]; int ac = 0;
        argv[ac++] = "curl"; argv[ac++] = "-s"; argv[ac++] = "-L";
        argv[ac++] = "-A"; argv[ac++] = "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36";
        argv[ac++] = "-b"; argv[ac++] = cin_tpl; argv[ac++] = "-c"; argv[ac++] = cout_tpl;
        argv[ac++] = "-X"; argv[ac++] = (char*)method;
        argv[ac++] = "-D"; argv[ac++] = hdr_tpl;
        argv[ac++] = "-o"; argv[ac++] = body_tpl;
        argv[ac++] = "--max-time"; argv[ac++] = "60";
        split_headers(headers_raw, argv, &ac, 60);
        char databin[160];
        if (req_fd >= 0) { snprintf(databin, sizeof databin, "@%s", req_tpl); argv[ac++] = "--data-binary"; argv[ac++] = databin; }
        argv[ac++] = (char*)url; argv[ac] = NULL;
        execvp("curl", argv);
        _exit(127);
    }
    if (streaming) {
        close(body_pipe[1]); body_pipe[1] = -1;
        int fl = fcntl(body_pipe[0], F_GETFL, 0);
        if (fl < 0 || fcntl(body_pipe[0], F_SETFL, fl | O_NONBLOCK) < 0) {
            close(body_pipe[0]); body_pipe[0] = -1;
            kill(pid, SIGKILL); int status = 0; (void)waitpid(pid, &status, 0);
            if (body_tpl[0]) unlink(body_tpl); unlink(hdr_tpl); unlink(side); unlink(cin_tpl); unlink(cout_tpl); if (req_fd >= 0) unlink(req_tpl);
            return 0;
        }
        out->body_fd = body_pipe[0]; body_pipe[0] = -1;
    } else out->body_fd = -1;
    out->used = 1; out->pid = pid; out->streaming = streaming;
    snprintf(out->monitor_url, sizeof out->monitor_url, "%s", url ? url : "");
    snprintf(out->monitor_method, sizeof out->monitor_method, "%s", method && *method ? method : "GET");
    out->monitor_started_ms = nal_monitor_now_ms();
    if (body_tpl[0]) snprintf(out->body_path, sizeof out->body_path, "%s", body_tpl);
    snprintf(out->hdr_path, sizeof out->hdr_path, "%s", hdr_tpl);
    return 1;
}

int nal_async_start_ex(const char* method, const char* url, const char* headers_raw, const char* body, size_t body_len, const NalFetchOpts* opts, NalAsyncReq* out) {
    return nal_async_start_impl(method, url, headers_raw, body, body_len, opts, 0, out);
}
int nal_async_start_stream_ex(const char* method, const char* url, const char* headers_raw, const char* body, size_t body_len, const NalFetchOpts* opts, NalAsyncReq* out) {
    return nal_async_start_impl(method, url, headers_raw, body, body_len, opts, 1, out);
}

static void stream_reap(NalAsyncReq* req) {
    if (!req || req->finished || req->pid <= 0) return;
    int status = 0; pid_t r = waitpid(req->pid, &status, WNOHANG);
    if (r == 0) return;
    req->finished = 1; req->pid = 0;
    if (r < 0) req->exit_code = 1;
    else req->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
}

int nal_async_stream_headers(NalAsyncReq* req, int* out_status, char** out_headers) {
    if (out_status) *out_status = 0; if (out_headers) *out_headers = NULL;
    if (!req || !req->used || !req->streaming || !out_status || !out_headers) return -1;
    stream_reap(req);
    struct stat st;
    if (stat(req->hdr_path, &st) == 0 && st.st_size > 0 && st.st_size < (1 << 20)) {
        FILE* f = fopen(req->hdr_path, "rb"); if (!f) return req->finished ? -1 : 0;
        char* h = malloc((size_t)st.st_size + 1); if (!h) { fclose(f); return -1; }
        size_t n = fread(h, 1, (size_t)st.st_size, f); fclose(f); h[n] = 0;
        if (n == 0) { free(h); return req->finished ? -1 : 0; }
        const char* sp = strchr(h, ' '); if (sp) *out_status = atoi(sp + 1);
        *out_headers = h; req->headers_sent = 1; return 1;
    }
    if (req->finished) return -1;
    return 0;
}

int nal_async_stream_read(NalAsyncReq* req, unsigned char* buffer, size_t capacity, size_t* out_read, int* out_done, int* out_error) {
    if (out_read) *out_read = 0; if (out_done) *out_done = 0; if (out_error) *out_error = 0;
    if (!req || !req->used || !req->streaming || req->body_fd < 0 || !buffer || capacity == 0 || !out_read || !out_done || !out_error) return -1;
    ssize_t n = read(req->body_fd, buffer, capacity);
    if (n > 0) { *out_read = (size_t)n; return 1; }
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) return -1;
    if (n == 0) {
        stream_reap(req);
        if (req->finished) { *out_done = 1; *out_error = req->exit_code != 0; return 1; }
    }
    return 0;
}

static void async_cleanup_files(NalAsyncReq* req, int import_cookies) {
    char side[160]; snprintf(side, sizeof side, "%s.aux", req->hdr_path);
    char cin_tpl[128] = {0}, cout_tpl[128] = {0}, req_tpl[128] = {0};
    FILE* sf = fopen(side, "r");
    if (sf) {
        char l1[128] = {0}, l2[128] = {0}, l3[128] = {0};
        (void)fscanf(sf, "%127[^\n]\n%127[^\n]\n%127[^\n]", l1, l2, l3); fclose(sf);
        snprintf(cin_tpl, sizeof cin_tpl, "%s", l1); snprintf(cout_tpl, sizeof cout_tpl, "%s", l2); snprintf(req_tpl, sizeof req_tpl, "%s", l3); unlink(side);
    }
    if (import_cookies && cout_tpl[0]) db_import_cookies_from_file(active_profile_id, cout_tpl);
    if (req->body_path[0]) unlink(req->body_path); unlink(req->hdr_path);
    { char ready[192]; snprintf(ready, sizeof ready, "%s.ready", req->hdr_path); unlink(ready); }
    if (cin_tpl[0]) unlink(cin_tpl); if (cout_tpl[0]) unlink(cout_tpl); if (req_tpl[0]) unlink(req_tpl);
}

void nal_async_stream_close(NalAsyncReq* req, int cancel) {
    if (!req || !req->used || !req->streaming) return;
    stream_reap(req);
    if (!req->finished && req->pid > 0) {
        kill(req->pid, SIGKILL); int status = 0; (void)waitpid(req->pid, &status, 0);
        req->pid = 0; req->finished = 1; req->exit_code = 128 + SIGKILL;
    }
    if (req->body_fd >= 0) { close(req->body_fd); req->body_fd = -1; }
    async_cleanup_files(req, !cancel && req->exit_code == 0);
    req->used = 0;
}

int nal_async_poll(NalAsyncReq* req, int* out_status, char** out_headers, char** out_body) {
    return nal_async_poll_ex(req, out_status, out_headers, out_body, NULL);
}

int nal_async_poll_ex(NalAsyncReq* req, int* out_status, char** out_headers, char** out_body, size_t* out_body_len) {
    if (!req->used) return 0;
    int status = 0; pid_t r = waitpid(req->pid, &status, WNOHANG);
    if (r == 0) return 0;                                          /* ещё выполняется */
    req->used = 0;

    char side[160]; snprintf(side, sizeof side, "%s.aux", req->hdr_path);
    char cin_tpl[128] = {0}, cout_tpl[128] = {0}, req_tpl[128] = {0};
    FILE* sf = fopen(side, "r");
    if (sf) { char l1[128] = {0}, l2[128] = {0}, l3[128] = {0}; if (fscanf(sf, "%127[^\n]\n%127[^\n]\n%127[^\n]", l1, l2, l3) >= 0) {} fclose(sf);
        snprintf(cin_tpl, sizeof cin_tpl, "%s", l1); snprintf(cout_tpl, sizeof cout_tpl, "%s", l2); snprintf(req_tpl, sizeof req_tpl, "%s", l3); unlink(side); }

    int ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    char* hdrs = NULL; char* body = NULL; size_t body_n = 0; int http_status = 0;
    if (ok) {
        FILE* hf = fopen(req->hdr_path, "rb");
        if (hf) { fseek(hf, 0, SEEK_END); long sz = ftell(hf); fseek(hf, 0, SEEK_SET); if (sz > 0 && sz < 1 << 20) { hdrs = malloc((size_t)sz + 1); size_t rd = fread(hdrs, 1, (size_t)sz, hf); hdrs[rd] = 0; } fclose(hf); }
        if (hdrs) { const char* sp = strchr(hdrs, ' '); if (sp) http_status = atoi(sp + 1); }
        FILE* bf = fopen(req->body_path, "rb");
        if (bf) { fseek(bf, 0, SEEK_END); long sz = ftell(bf); fseek(bf, 0, SEEK_SET); if (sz < 0) sz = 0; body = malloc((size_t)sz + 1); if (body) { size_t rd = fread(body, 1, (size_t)sz, bf); body[rd] = 0; body_n = rd; } fclose(bf); }
        if (cout_tpl[0]) db_import_cookies_from_file(active_profile_id, cout_tpl);
    }
    unlink(req->body_path); unlink(req->hdr_path);
    if (cin_tpl[0]) unlink(cin_tpl); if (cout_tpl[0]) unlink(cout_tpl); if (req_tpl[0]) unlink(req_tpl);

    *out_status = ok ? http_status : 0; *out_headers = hdrs ? hdrs : strdup(""); *out_body = body ? body : strdup(""); if (out_body_len) *out_body_len = body ? body_n : 0;
    nal_monitor_record(req->monitor_method[0] ? req->monitor_method : "GET", req->monitor_url,
                       ok ? http_status : 0, req->monitor_started_ms ? nal_monitor_now_ms() - req->monitor_started_ms : 0,
                       body ? (long long)body_n : 0, "fetch/xhr");
    return 1;
}

void nal_async_cancel(NalAsyncReq* req) {
    if (!req->used) return;
    if (req->streaming) { nal_async_stream_close(req, 1); return; }
    kill(req->pid, SIGKILL); int status; waitpid(req->pid, &status, 0);
    char side[160]; snprintf(side, sizeof side, "%s.aux", req->hdr_path);
    FILE* sf = fopen(side, "r");
    if (sf) { char l1[128] = {0}, l2[128] = {0}, l3[128] = {0}; if (fscanf(sf, "%127[^\n]\n%127[^\n]\n%127[^\n]", l1, l2, l3) >= 0) {} fclose(sf);
        if (l1[0]) unlink(l1); if (l2[0]) unlink(l2); if (l3[0]) unlink(l3); unlink(side); }
    unlink(req->body_path); unlink(req->hdr_path);
    req->used = 0;
}
