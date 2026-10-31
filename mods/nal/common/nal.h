#ifndef NAL_H
#define NAL_H

#include <stddef.h>
#include <sys/types.h>
#include "web_security.h"

/* Network Abstraction Layer (NAL) */

// Инициализация сетевой подсистемы (особенно важно для Windows/Winsock)
int nal_init(void);

/* Local DevTools instrumentation. Logs metadata only (no request/response bodies or cookie values). */
void nal_monitor_record(const char* method, const char* url, int status, long long duration_ms, long long bytes, const char* kind);
void nal_monitor_set_last_response(const char* headers, const char* url, int status);
int nal_get_last_response_metadata(char* headers, size_t headers_cap, char* url, size_t url_cap, int* status);
long long nal_monitor_now_ms(void);
void nal_monitor_clear(void);

// Завершение работы с сетью
void nal_close(void);

/* HTTP-кэш (Linux: собственный клиент, см. mods/nal/README.md). Политика действует на запросы, начатые после вызова
   (процессы загрузки наследуют её при fork). На других платформах функции — заглушки. */
enum { NAL_CACHE_NORMAL = 0, NAL_CACHE_RELOAD = 1, NAL_CACHE_OFFLINE = 2 };   /* RELOAD — не доверять кэшу (кнопка «Обновить»), OFFLINE — только кэш */
void nal_cache_policy(int policy);
void nal_cache_clear(void);

// Загрузка содержимого по URL в память. Возвращает malloc'ированную строку или NULL.
char* nal_http_get(const char* url);
char* nal_http_get_kind(const char* url, int resource_kind);
void nal_set_document_url(const char* url);
const char* nal_current_document_url(void);
int nal_security_allows(const char* url, int resource_kind);

// Загрузка файла по URL и сохранение по пути dest_path.
// Возвращает 1 при успехе, 0 при ошибке.
int nal_download_file(const char* url, const char* dest_path);
int nal_download_file_kind(const char* url, const char* dest_path, int resource_kind);
/* Resume an interrupted DB download by ID. Requires a validated Range/If-Range response; keeps partial data on transient failure. */
int nal_resume_download(int download_id);

/* Linux background download controller. start returns a database download ID or -1.
   poll refreshes byte counts and final states; pause/continue/cancel return 1 on success. */
int nal_download_start(const char* url, const char* dest_path, const char* filename);
int nal_download_pause(int download_id);
int nal_download_continue(int download_id);
int nal_download_cancel(int download_id);
/* Queue a validated resume in the background; returns the download ID or -1. */
int nal_download_resume_start(int download_id);
void nal_download_poll(void);

/* ── Асинхронные запросы (fetch/XHR любых методов) ──
   Не блокируют вызывающий процесс: запрос выполняется в отдельном процессе (fork+exec curl),
   готовность проверяется неблокирующим nal_async_poll() из основного цикла событий. */
typedef struct {
    int used;
    pid_t pid;
    int streaming, headers_sent, finished, exit_code;
    int body_fd;                         /* Linux streaming: nonblocking read end of bounded pipe; -1 otherwise */
    size_t body_offset;
    char body_path[128], hdr_path[128];
    /* Developer Tools Network instrumentation; does not retain request bodies or credentials. */
    char monitor_url[2048], monitor_method[16];
    long long monitor_started_ms;
} NalAsyncReq;

/* method: "GET"/"POST"/... ; headers_raw: строки "Ключ: значение", разделённые '\n', может быть NULL;
   body/body_len: тело запроса (может быть NULL/0). Возвращает 1 и заполняет *out при успешном запуске, иначе 0. */
int nal_async_start(const char* method, const char* url, const char* headers_raw, const char* body, size_t body_len, NalAsyncReq* out);

/* Запрос из скрипта страницы с политикой одного источника и CORS (Linux: mods/nal/common/ncors.c). page_url == NULL — доверенный запрос
   браузера без проверок. credentials: 0 omit / 1 same-origin / 2 include; mode: 0 cors / 1 no-cors / 2 same-origin. Нарушение политики —
   как сетевая ошибка (status 0). На платформах без реализации действует прежнее поведение nal_async_start. */
/* redirect: 0 follow (default), 1 manual, 2 error. */
typedef struct { const char* page_url; int credentials; int mode; int redirect; } NalFetchOpts;

/* Synchronous request bridge used by legacy XMLHttpRequest.open(..., false).
   Body is binary-safe and owned by the response until nal_sync_response_free().
   Backends that cannot preserve browser CORS semantics may return an explicit unsupported error. */
#define NAL_SYNC_URL_MAX 8700
typedef struct {
    int status;
    char* headers;
    unsigned char* body;
    size_t body_len;
    char final_url[NAL_SYNC_URL_MAX];
    char error[200];
} NalSyncResponse;
int nal_sync_fetch_ex(const char* method, const char* url, const char* headers_raw,
                      const unsigned char* body, size_t body_len, const NalFetchOpts* opts,
                      NalSyncResponse* out);
void nal_sync_response_free(NalSyncResponse* response);

/* Смешанное содержимое: страница по https не должна подгружать «активные» ресурсы (скрипты, стили, шрифты, fetch) по http.
   Исключение — loopback (localhost, 127.x, ::1): разработка. 1 — загрузка должна быть запрещена. */
#include <strings.h>
#include <string.h>
static inline int nal_loopback_authority(const char* authority, size_t len) {
    if (!authority || !len || len >= 256) return 0;
    char host[256];
    if (authority[0] == '[') {
        const char* close = memchr(authority, ']', len);
        if (!close) return 0;
        size_t n = (size_t)(close - authority - 1);
        if (n == 0 || n >= sizeof host) return 0;
        memcpy(host, authority + 1, n); host[n] = 0;
        return !strcasecmp(host, "::1");
    }
    const char* at = NULL;
    for (size_t i = 0; i < len; i++) if (authority[i] == '@') at = authority + i;
    if (at) { len -= (size_t)(at + 1 - authority); authority = at + 1; }
    size_t hn = len;
    for (size_t i = 0; i < hn; i++) if (authority[i] == ':') { hn = i; break; }
    if (!hn || hn >= sizeof host) return 0;
    memcpy(host, authority, hn); host[hn] = 0;
    if (!strcasecmp(host, "localhost") || !strcasecmp(host, "localhost.")) return 1;
    unsigned octets[4] = {0}; size_t part = 0; unsigned digits = 0;
    for (size_t i = 0; i <= hn; i++) {
        char c = i == hn ? '.' : host[i];
        if (c >= '0' && c <= '9') {
            if (++digits > 3) return 0;
            octets[part] = octets[part] * 10u + (unsigned)(c - '0');
            if (octets[part] > 255) return 0;
        } else if (c == '.' && digits && part < 4) {
            part++; digits = 0;
        } else return 0;
    }
    return part == 4 && octets[0] == 127;
}
static inline int nal_mixed_content_blocked(const char* page_url, const char* url) {
    if (!page_url || !url || strncasecmp(page_url, "https://", 8) != 0 || strncasecmp(url, "http://", 7) != 0) return 0;
    const char* a = url + 7;
    size_t n = strcspn(a, "/?#");
    if (nal_loopback_authority(a, n)) return 0;
    return 1;
}

int nal_async_start_ex(const char* method, const char* url, const char* headers_raw, const char* body, size_t body_len, const NalFetchOpts* opts, NalAsyncReq* out);
/* Streaming fetch backend: resolves headers before the body completes; Linux reads through a bounded OS pipe with backpressure. */
int nal_async_start_stream_ex(const char* method, const char* url, const char* headers_raw, const char* body, size_t body_len, const NalFetchOpts* opts, NalAsyncReq* out);
int nal_async_stream_headers(NalAsyncReq* req, int* out_status, char** out_headers); /* 1 headers, 0 pending, -1 request failed before headers */
int nal_async_stream_read(NalAsyncReq* req, unsigned char* buffer, size_t capacity, size_t* out_read, int* out_done, int* out_error); /* 1 available, 0 pending, -1 I/O error */
void nal_async_stream_close(NalAsyncReq* req, int cancel);

/* Неблокирующая проверка готовности. Возвращает: 0 — ещё выполняется, 1 — завершён (успешно или с сетевой ошибкой).
   При возврате 1 заполняет *out_status (0 при сетевой ошибке/обрыве), *out_headers и *out_body (malloc'ированные, может быть пустая строка;
   вызывающий обязан free()); также освобождает временные файлы. */
int nal_async_poll(NalAsyncReq* req, int* out_status, char** out_headers, char** out_body);

/* То же, что nal_async_poll, но возвращает ещё и длину тела (*out_body_len): тело бинарное и может содержать '\0'.
   *out_body всегда с завершающим '\0' (не входит в длину). */
int nal_async_poll_ex(NalAsyncReq* req, int* out_status, char** out_headers, char** out_body, size_t* out_body_len);

/* Отменяет ещё не завершённый запрос (SIGKILL дочернему процессу) и подчищает временные файлы. */
void nal_async_cancel(NalAsyncReq* req);

#endif
