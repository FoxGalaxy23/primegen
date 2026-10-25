#include "nal.h"
#include "gal.h"
#include "db.h"
#include "nhttp.h"
#include "ncors.h"
#include "ntls.h"
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

int nal_init(void) {
    if (!getenv("MLBROWSER_NO_CACHE")) nhttp_cache_config("cache/http", (size_t)256 << 20);       /* HTTP-кэш на диске, до 256 МиБ (LRU) */
    return 1;
}

void nal_cache_policy(int policy) { nhttp_cache_policy(policy); }
void nal_cache_clear(void) { nhttp_cache_clear(); }

void nal_close(void) {
    nhttp_pool_clear();
}

static char* curl_http_get(const char* url);
static int curl_download_file(const char* url, const char* dest_path);

char* nal_http_get(const char* url) {
    if (!use_native(url)) return curl_http_get(url);
    gal_log("INFO", "NAL: fetching URL (native): '%s'", url);
    NJar* jar = jar_from_db();
    NHttpReq rq; memset(&rq, 0, sizeof rq); rq.url = url; rq.jar = jar;
    NHttpResp rs;
    if (!nhttp_perform(&rq, &rs)) {
        gal_log("ERROR", "NAL: request failed: %s (%s)", rs.err, url);
        nhttp_resp_free(&rs); njar_free(jar); return NULL;
    }
    if (rs.from_cache) gal_log("INFO", "NAL: %s из кэша: '%s'", rs.from_cache == 1 ? "свежая запись" : "подтверждено (304)", url);
    if (jar) { jar_to_db(jar); njar_free(jar); }
    char* body = (char*)rs.body; rs.body = NULL;               /* владение переходит вызывающему; тело всегда завершено '\0' */
    nhttp_resp_free(&rs);
    return body;
}

int nal_download_file(const char* url, const char* dest_path) {
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

static char* curl_http_get(const char* url) {
    gal_log("INFO", "NAL: fetching URL (curl): '%s'", url);

    mkdir("cache", 0777);

    char temp_page[] = "cache/temp_page_XXXXXX";
    int page_fd = mkstemp(temp_page);
    if (page_fd < 0) {
        gal_log("ERROR", "NAL: failed to create temp page file");
        return NULL;
    }
    close(page_fd);

    char cookie_in[] = "cache/cookies_in_XXXXXX";
    int cin_fd = mkstemp(cookie_in);
    if (cin_fd >= 0) close(cin_fd);

    char cookie_out[] = "cache/cookies_out_XXXXXX";
    int cout_fd = mkstemp(cookie_out);
    if (cout_fd >= 0) close(cout_fd);

    // Экспорт куки
    db_export_cookies_to_file(active_profile_id, cookie_in);

    // Экранирование URL для shell
    char sanitized_url[1024] = {0};
    int j = 0;
    for (int i = 0; url[i] != '\0' && j < 1000; i++) {
        if (url[i] == '\'') {
            strcpy(sanitized_url + j, "'\\''");
            j += 4;
        } else {
            sanitized_url[j++] = url[i];
        }
    }

    char cmd[3072];
    snprintf(cmd, sizeof(cmd),
             "curl -s -L -A \"Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36\" "
             "-b \"%s\" -c \"%s\" -o \"%s\" '%s'",
             cookie_in, cookie_out, temp_page, sanitized_url);

    gal_log("INFO", "NAL: executing: %s", cmd);
    int ret = system(cmd);

    if (ret != 0) {
        gal_log("ERROR", "NAL: curl failed with code %d", ret);
        unlink(temp_page);
        unlink(cookie_in);
        unlink(cookie_out);
        return NULL;
    }

    // Импорт куки обратно
    db_import_cookies_from_file(active_profile_id, cookie_out);

    FILE* f = fopen(temp_page, "r");
    char* buffer = NULL;
    if (f) {
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        buffer = malloc(size + 1);
        if (buffer) {
            size_t read_bytes = fread(buffer, 1, size, f);
            buffer[read_bytes] = '\0';
        }
        fclose(f);
    }

    unlink(temp_page);
    unlink(cookie_in);
    unlink(cookie_out);

    return buffer;
}

static int curl_download_file(const char* url, const char* dest_path) {
    gal_log("INFO", "NAL: downloading file (curl): '%s' -> '%s'", url, dest_path);
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "curl -s -L -A \"Mozilla/5.0\" -o \"%s\" \"%s\"", dest_path, url);
    int ret = system(cmd);
    return (ret == 0);
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

/* Тело потомка для асинхронного запроса (после fork): весь сетевой обмен делаем нативным клиентом и пишем результат в те же файлы,
   что раньше писал curl. В файл заголовков идёт только финальный блок (редиректы уже пройдены), так что статус разбирается верно. */
static int native_async_child(const char* method, const char* url, const char* headers_raw, const char* req_path,
                              const char* cin, const char* cout, const char* hdr_path, const char* body_path, const NalFetchOpts* opts) {
    unsigned char* body = NULL; size_t blen = 0;
    if (req_path && req_path[0]) {
        FILE* rf = fopen(req_path, "rb");
        if (rf) { fseek(rf, 0, SEEK_END); long sz = ftell(rf); fseek(rf, 0, SEEK_SET); if (sz > 0) { body = malloc((size_t)sz); if (body) blen = fread(body, 1, (size_t)sz, rf); } fclose(rf); }
    }
    NJar* jar = njar_new(); if (jar && cin) njar_load_netscape(jar, cin);
    NCorsReq cr; memset(&cr, 0, sizeof cr); cr.method = method; cr.url = url; cr.headers = headers_raw; cr.body = body; cr.body_len = blen; cr.jar = jar; cr.timeout_ms = 60000;
    if (opts && opts->page_url) { cr.page_url = opts->page_url; cr.credentials = opts->credentials; cr.mode = opts->mode; }     /* запрос скрипта: SOP/CORS */
    NHttpResp rs; int ok = ncors_fetch(&cr, &rs);
    if (!ok && opts && opts->page_url) gal_log("WARN", "fetch заблокирован/не удался (%s → %s): %s", opts->page_url, url, rs.err);
    if (ok) {
        FILE* hf = fopen(hdr_path, "wb"); if (hf) { if (rs.headers) fputs(rs.headers, hf); fputs("\r\n", hf); fclose(hf); }
        FILE* bf = fopen(body_path, "wb"); if (bf) { if (rs.body_len) fwrite(rs.body, 1, rs.body_len, bf); fclose(bf); }
        if (jar && cout && cr.jar_used) njar_save_changed(jar, cout);                 /* куки сохраняем, только если политика разрешила их использовать */
    }
    nhttp_resp_free(&rs); njar_free(jar); free(body);
    return ok ? 0 : 1;
}

int nal_async_start(const char* method, const char* url, const char* headers_raw, const char* body, size_t body_len, NalAsyncReq* out) {
    return nal_async_start_ex(method, url, headers_raw, body, body_len, NULL, out);
}

int nal_async_start_ex(const char* method, const char* url, const char* headers_raw, const char* body, size_t body_len, const NalFetchOpts* opts, NalAsyncReq* out) {
    memset(out, 0, sizeof *out);
    if (opts && opts->page_url && !use_native(url)) return 0;            /* политику CORS обеспечивает только собственный клиент — запасной curl для запросов скриптов не используем */
    mkdir("cache", 0777);
    char body_tpl[] = "cache/areq_b_XXXXXX", hdr_tpl[] = "cache/areq_h_XXXXXX", cin_tpl[] = "cache/areq_ci_XXXXXX", cout_tpl[] = "cache/areq_co_XXXXXX", req_tpl[] = "cache/areq_rq_XXXXXX";
    int bf = mkstemp(body_tpl); int hf = mkstemp(hdr_tpl); int cif = mkstemp(cin_tpl); int cof = mkstemp(cout_tpl);
    if (bf < 0 || hf < 0 || cif < 0 || cof < 0) { if (bf >= 0) { close(bf); unlink(body_tpl); } if (hf >= 0) { close(hf); unlink(hdr_tpl); } if (cif >= 0) { close(cif); unlink(cin_tpl); } if (cof >= 0) { close(cof); unlink(cout_tpl); } return 0; }
    close(bf); close(hf); close(cof);
    db_export_cookies_to_file(active_profile_id, cin_tpl); close(cif);

    int req_fd = -1;
    if (body && body_len) { req_fd = mkstemp(req_tpl); if (req_fd >= 0) { ssize_t w = write(req_fd, body, body_len); (void)w; close(req_fd); } }

    pid_t pid = fork();
    if (pid < 0) { unlink(body_tpl); unlink(hdr_tpl); unlink(cin_tpl); unlink(cout_tpl); if (req_fd >= 0) unlink(req_tpl); return 0; }
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY); if (devnull >= 0) { dup2(devnull, 1); dup2(devnull, 2); }
        if (use_native(url)) _exit(native_async_child(method, url, headers_raw, req_fd >= 0 ? req_tpl : NULL, cin_tpl, cout_tpl, hdr_tpl, body_tpl, opts));
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
    out->used = 1; out->pid = pid;
    snprintf(out->body_path, sizeof out->body_path, "%s", body_tpl);
    snprintf(out->hdr_path, sizeof out->hdr_path, "%s", hdr_tpl);
    /* cookie-файлы подчищаем сразу после запуска ребёнка: cin больше не нужен ребёнку (curl уже открыл на чтение при -b, но чтобы
       не плодить файлы на долгих запросах — оставляем cin/cout только в приватных статических путях запроса, храним имена в hdr/body-соседях */
    { char* extra = malloc(400); snprintf(extra, 400, "%s\n%s\n%s", cin_tpl, cout_tpl, req_fd >= 0 ? req_tpl : ""); /* временно пишем в отдельный "теневой" файл рядом с hdr, чтобы poll() потом убрал их и импортировал куки */
      char side[160]; snprintf(side, sizeof side, "%s.aux", hdr_tpl); FILE* sf = fopen(side, "w"); if (sf) { fputs(extra, sf); fclose(sf); } free(extra); }
    return 1;
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
    return 1;
}

void nal_async_cancel(NalAsyncReq* req) {
    if (!req->used) return;
    kill(req->pid, SIGKILL); int status; waitpid(req->pid, &status, 0);
    char side[160]; snprintf(side, sizeof side, "%s.aux", req->hdr_path);
    FILE* sf = fopen(side, "r");
    if (sf) { char l1[128] = {0}, l2[128] = {0}, l3[128] = {0}; if (fscanf(sf, "%127[^\n]\n%127[^\n]\n%127[^\n]", l1, l2, l3) >= 0) {} fclose(sf);
        if (l1[0]) unlink(l1); if (l2[0]) unlink(l2); if (l3[0]) unlink(l3); unlink(side); }
    unlink(req->body_path); unlink(req->hdr_path);
    req->used = 0;
}
