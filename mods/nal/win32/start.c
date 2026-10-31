#include "nal.h"
#include "gal.h"
#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>

#pragma comment(lib, "winhttp.lib")

static HINTERNET hSession = NULL;

int nal_init(void) {
    hSession = WinHttpOpen(L"Meetlookers Browser/1.0",
                           WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                           WINHTTP_NO_PROXY_NAME,
                           WINHTTP_NO_PROXY_BYPASS, 0);
    return (hSession != NULL);
}

void nal_cache_policy(int policy) { (void)policy; }      /* HTTP-кэш пока только в Linux-клиенте */
void nal_cache_clear(void) {}

void nal_sync_response_free(NalSyncResponse* response) {
    if (!response) return; free(response->headers); free(response->body); memset(response, 0, sizeof *response);
}

void nal_close(void) {
    if (hSession) {
        WinHttpCloseHandle(hSession);
        hSession = NULL;
    }
}

/* Background download process control is currently Linux-only. */
int nal_download_start(const char* url, const char* path, const char* filename) { (void)url; (void)path; (void)filename; return -1; }
int nal_download_pause(int id) { (void)id; return 0; }
int nal_download_continue(int id) { (void)id; return 0; }
int nal_download_cancel(int id) { (void)id; return 0; }
int nal_download_resume_start(int id) { (void)id; return -1; }
void nal_download_poll(void) {}
int nal_resume_download(int id) { (void)id; return 0; }

static WCHAR* char_to_wchar(const char* s) {
    int len = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    WCHAR* ws = malloc(len * sizeof(WCHAR));
    MultiByteToWideChar(CP_UTF8, 0, s, -1, ws, len);
    return ws;
}

static char g_nal_doc_url[8192];

void nal_set_document_url(const char* url) {
    char upgraded[16384];
    const char* use = url ? url : "";
    if (*use && websec_hsts_upgrade_url(use, upgraded, sizeof upgraded)) use = upgraded;
    size_t n = strlen(use);
    if (n >= sizeof g_nal_doc_url) g_nal_doc_url[0] = 0;
    else memcpy(g_nal_doc_url, use, n + 1);
    websec_set_document_url(use);
}

const char* nal_current_document_url(void) { return g_nal_doc_url; }

int nal_security_allows(const char* url, int resource_kind) {
    if (!url || !*url) return 0;
    if (resource_kind == NAL_RESOURCE_DOCUMENT) return 1;
    return websec_allows(url, resource_kind);
}

static char* winhttp_response_headers(HINTERNET hRequest);

/* Small URL-origin helper for the Win32 XHR bridge. Cross-origin requests fail closed until
   the WinHTTP backend has a complete preflight/CORS implementation. */
static void win_origin(const char* url, char* out, size_t cap) {
    if (!out || !cap) return;
    out[0] = 0;
    if (!url) return;
    const char* sep = strstr(url, "://");
    if (!sep) return;
    char scheme[16]; size_t sn = (size_t)(sep - url);
    if (!sn || sn >= sizeof scheme) return;
    for (size_t i = 0; i < sn; i++) scheme[i] = (char)tolower((unsigned char)url[i]);
    scheme[sn] = 0;
    const char* auth = sep + 3; const char* end = auth + strcspn(auth, "/?#");
    const char* at = NULL; for (const char* p = auth; p < end; p++) if (*p == '@') at = p;
    if (at) auth = at + 1;
    if (auth >= end) return;
    char hostport[512]; size_t hn = (size_t)(end - auth);
    if (hn >= sizeof hostport) return;
    memcpy(hostport, auth, hn); hostport[hn] = 0;
    char* portstr = NULL;
    if (hostport[0] == '[') {
        char* close = strchr(hostport, ']'); if (!close) return;
        if (close[1] == ':') { portstr = close + 2; close[1] = 0; }
        else if (close[1]) return;
    } else {
        char* colon = strrchr(hostport, ':');
        if (colon) {
            if (strchr(hostport, ':') != colon) return; /* unbracketed IPv6 authority */
            portstr = colon + 1; *colon = 0;
        }
    }
    if (portstr) {
        if (!*portstr) return;
        char* e = NULL; unsigned long port = strtoul(portstr, &e, 10);
        if (!e || *e || port > 65535 || port == 0) return;
        if ((!strcmp(scheme, "http") && port == 80) || (!strcmp(scheme, "https") && port == 443)) portstr = NULL;
    }
    for (char* p = hostport; *p; p++) *p = (char)tolower((unsigned char)*p);
    if (portstr) snprintf(out, cap, "%s://%s:%s", scheme, hostport, portstr);
    else snprintf(out, cap, "%s://%s", scheme, hostport);
}
static int win_same_origin(const char* a, const char* b) {
    char ao[600], bo[600]; win_origin(a, ao, sizeof ao); win_origin(b, bo, sizeof bo);
    return ao[0] && bo[0] && !strcmp(ao, bo);
}
int nal_sync_fetch_ex(const char* method, const char* url, const char* headers_raw,
                      const unsigned char* body, size_t body_len, const NalFetchOpts* opts,
                      NalSyncResponse* out) {
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    if (!url || !*url || (body_len && !body) || body_len > (size_t)256 * 1024 * 1024 || body_len > MAXDWORD) {
        snprintf(out->error, sizeof out->error, "invalid URL or request body too large"); return 0;
    }
    char upgraded[NAL_SYNC_URL_MAX]; const char* effective = url;
    if (websec_hsts_upgrade_url(url, upgraded, sizeof upgraded)) effective = upgraded;
    if (opts && opts->page_url) {
        if (!nal_security_allows(effective, NAL_RESOURCE_CONNECT)) {
            snprintf(out->error, sizeof out->error, "CSP blocked synchronous request"); return 0;
        }
        if (!win_same_origin(opts->page_url, effective)) {
            snprintf(out->error, sizeof out->error, "Win32 synchronous XHR currently blocks cross-origin requests (CORS preflight is not implemented)"); return 0;
        }
    }
    if (!hSession && !nal_init()) { snprintf(out->error, sizeof out->error, "WinHTTP initialization failed"); return 0; }
    WCHAR* wurl = char_to_wchar(effective); if (!wurl) { snprintf(out->error, sizeof out->error, "out of memory"); return 0; }
    URL_COMPONENTS uc; ZeroMemory(&uc, sizeof uc); uc.dwStructSize = sizeof uc;
    uc.dwSchemeLength = uc.dwHostNameLength = uc.dwUrlPathLength = uc.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(wurl, 0, 0, &uc) || (uc.nScheme != INTERNET_SCHEME_HTTP && uc.nScheme != INTERNET_SCHEME_HTTPS)) {
        free(wurl); snprintf(out->error, sizeof out->error, "unsupported URL"); return 0;
    }
    size_t pathn = uc.dwUrlPathLength ? uc.dwUrlPathLength : 1, extran = uc.dwExtraInfoLength;
    WCHAR* path = (WCHAR*)calloc(pathn + extran + 2, sizeof(WCHAR));
    WCHAR* host = (WCHAR*)calloc((size_t)uc.dwHostNameLength + 1, sizeof(WCHAR));
    if (!path || !host) { free(path); free(host); free(wurl); snprintf(out->error, sizeof out->error, "out of memory"); return 0; }
    if (uc.dwUrlPathLength) memcpy(path, uc.lpszUrlPath, uc.dwUrlPathLength * sizeof(WCHAR)); else path[0] = L'/';
    if (extran) { size_t off = wcslen(path); memcpy(path + off, uc.lpszExtraInfo, extran * sizeof(WCHAR)); path[off + extran] = 0; }
    WCHAR* fragment = wcschr(path, L'#'); if (fragment) *fragment = 0;
    memcpy(host, uc.lpszHostName, uc.dwHostNameLength * sizeof(WCHAR)); host[uc.dwHostNameLength] = 0;
    WCHAR* wm = char_to_wchar(method && *method ? method : "GET");
    if (!wm) { free(path); free(host); free(wurl); snprintf(out->error, sizeof out->error, "out of memory"); return 0; }
    HINTERNET connect = WinHttpConnect(hSession, host, uc.nPort, 0); HINTERNET request = NULL;
    if (connect) request = WinHttpOpenRequest(connect, wm, path, NULL, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    free(wm); free(path); free(host);
    if (!request) { if (connect) WinHttpCloseHandle(connect); free(wurl); snprintf(out->error, sizeof out->error, "WinHTTP could not create request"); return 0; }
#ifdef WINHTTP_OPTION_DECOMPRESSION
    { DWORD flags = 0;
#ifdef WINHTTP_DECOMPRESSION_FLAG_GZIP
      flags |= WINHTTP_DECOMPRESSION_FLAG_GZIP;
#endif
#ifdef WINHTTP_DECOMPRESSION_FLAG_DEFLATE
      flags |= WINHTTP_DECOMPRESSION_FLAG_DEFLATE;
#endif
#ifdef WINHTTP_DECOMPRESSION_FLAG_BROTLI
      flags |= WINHTTP_DECOMPRESSION_FLAG_BROTLI;
#endif
      if (flags) WinHttpSetOption(request, WINHTTP_OPTION_DECOMPRESSION, &flags, sizeof flags); }
#endif
    WCHAR* wh = NULL; DWORD whn = 0;
    if (headers_raw && *headers_raw) {
        size_t n = strlen(headers_raw); char* normalized = (char*)malloc(n * 2 + 4);
        if (!normalized) { WinHttpCloseHandle(request); WinHttpCloseHandle(connect); free(wurl); snprintf(out->error, sizeof out->error, "out of memory"); return 0; }
        size_t o = 0; const char* p = headers_raw;
        while (*p) { const char* e = strchr(p, '\n'); size_t ln = e ? (size_t)(e-p) : strlen(p); while (ln && p[ln-1] == '\r') ln--;
            int safe = ln && memchr(p, '\r', ln) == NULL && memchr(p, '\n', ln) == NULL && memchr(p, 0, ln) == NULL;
            if (safe && memchr(p, ':', ln)) {
                size_t name_n = 0; while (name_n < ln && p[name_n] != ':') name_n++;
                int forbidden = (name_n == 4 && !_strnicmp(p, "Host", 4)) || (name_n == 14 && !_strnicmp(p, "Content-Length", 14)) || (name_n == 17 && !_strnicmp(p, "Transfer-Encoding", 17));
                if (!forbidden) { memcpy(normalized + o, p, ln); o += ln; normalized[o++]='\r'; normalized[o++]='\n'; }
            }
            if (!e) break; p = e + 1;
        }
        normalized[o] = 0; wh = char_to_wchar(normalized); free(normalized);
        if (!wh) { WinHttpCloseHandle(request); WinHttpCloseHandle(connect); free(wurl); snprintf(out->error, sizeof out->error, "out of memory"); return 0; }
        whn = (DWORD)wcslen(wh);
    }
    BOOL sent = WinHttpSendRequest(request, wh ? wh : WINHTTP_NO_ADDITIONAL_HEADERS, wh ? whn : 0,
        body_len ? (LPVOID)body : WINHTTP_NO_REQUEST_DATA, (DWORD)body_len, (DWORD)body_len, 0);
    free(wh); int ok = sent && WinHttpReceiveResponse(request, NULL);
    if (ok) {
        DWORD status = 0, sz = sizeof status;
        if (WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
              WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX)) out->status = (int)status;
        out->headers = winhttp_response_headers(request);
        size_t used = 0, cap = 0; unsigned char chunk[16384];
        while (ok) { DWORD avail = 0; if (!WinHttpQueryDataAvailable(request, &avail)) { ok = 0; break; } if (!avail) break;
            DWORD want = avail > sizeof chunk ? (DWORD)sizeof chunk : avail, got = 0;
            if (!WinHttpReadData(request, chunk, want, &got)) { ok = 0; break; } if (!got) break;
            if ((size_t)got > ((size_t)256 << 20) - used) { ok = 0; snprintf(out->error, sizeof out->error, "response body too large"); break; }
            if (used + got + 1 > cap) { size_t nc = cap ? cap : 32768; while (nc < used + got + 1) nc *= 2; unsigned char* nb = (unsigned char*)realloc(out->body, nc); if (!nb) { ok = 0; snprintf(out->error, sizeof out->error, "out of memory"); break; } out->body = nb; cap = nc; }
            memcpy(out->body + used, chunk, got); used += got;
        }
        out->body_len = used; if (out->body) out->body[used] = 0;
        if (!out->headers) { out->headers = strdup(""); if (!out->headers) ok = 0; }
        /* Preserve the final URL after WinHTTP's normal redirect handling where supported. */
        DWORD need = 0; (void)WinHttpQueryOption(request, WINHTTP_OPTION_URL, NULL, &need);
        if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && need >= sizeof(WCHAR) && need < 65536) {
            WCHAR* fw = (WCHAR*)malloc(need + sizeof(WCHAR)); if (fw && WinHttpQueryOption(request, WINHTTP_OPTION_URL, fw, &need)) {
                fw[need / sizeof(WCHAR)] = 0; int bytes = WideCharToMultiByte(CP_UTF8, 0, fw, -1, NULL, 0, NULL, NULL);
                if (bytes > 0 && (size_t)bytes < sizeof out->final_url) WideCharToMultiByte(CP_UTF8, 0, fw, -1, out->final_url, bytes, NULL, NULL);
            } free(fw);
        }
        if (!out->final_url[0]) { int bytes = WideCharToMultiByte(CP_UTF8, 0, wurl, -1, NULL, 0, NULL, NULL); if (bytes > 0 && (size_t)bytes < sizeof out->final_url) WideCharToMultiByte(CP_UTF8, 0, wurl, -1, out->final_url, bytes, NULL, NULL); }
        if (opts && opts->page_url && out->final_url[0] && !win_same_origin(opts->page_url, out->final_url)) {
            ok = 0; snprintf(out->error, sizeof out->error, "Win32 synchronous XHR redirected cross-origin; blocked by policy");
        }
    } else if (!out->error[0]) snprintf(out->error, sizeof out->error, "WinHTTP request failed");
    WinHttpCloseHandle(request); WinHttpCloseHandle(connect); free(wurl);
    if (!ok) { char saved[sizeof out->error]; snprintf(saved, sizeof saved, "%s", out->error); nal_sync_response_free(out); snprintf(out->error, sizeof out->error, "%s", saved[0] ? saved : "WinHTTP request failed"); return 0; }
    return 1;
}

static char* winhttp_response_headers(HINTERNET hRequest) {
    DWORD bytes = 0;
    SetLastError(ERROR_SUCCESS);
    (void)WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_RAW_HEADERS_CRLF,
        WINHTTP_HEADER_NAME_BY_INDEX, NULL, &bytes, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes < sizeof(WCHAR) || bytes > 1024 * 1024) return NULL;
    WCHAR* wide = (WCHAR*)malloc(bytes + sizeof(WCHAR));
    if (!wide) return NULL;
    if (!WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_RAW_HEADERS_CRLF,
        WINHTTP_HEADER_NAME_BY_INDEX, wide, &bytes, WINHTTP_NO_HEADER_INDEX)) { free(wide); return NULL; }
    wide[bytes / sizeof(WCHAR)] = L'\0';
    int needed = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
    if (needed <= 0) { free(wide); return NULL; }
    char* utf8 = (char*)malloc((size_t)needed);
    if (!utf8 || !WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, needed, NULL, NULL)) { free(utf8); utf8 = NULL; }
    free(wide);
    return utf8;
}

static char* nal_http_get_unchecked(const char* url) {
    long long started = nal_monitor_now_ms();
    int is_document = websec_is_document_url(url);
    int response_status = 0;
    gal_log("INFO", "NAL: fetching URL (Win32): %s", url);
    if (!hSession) return NULL;

    URL_COMPONENTS urlComp;
    ZeroMemory(&urlComp, sizeof(urlComp));
    urlComp.dwStructSize = sizeof(urlComp);
    urlComp.dwHostNameLength = (DWORD)-1;
    urlComp.dwUrlPathLength = (DWORD)-1;
    urlComp.dwExtraInfoLength = (DWORD)-1;

    WCHAR* wurl = char_to_wchar(url);
    if (!WinHttpCrackUrl(wurl, (DWORD)wcslen(wurl), 0, &urlComp)) {
        free(wurl);
        return NULL;
    }

    WCHAR szHost[256];
    if (urlComp.dwHostNameLength >= 256) { free(wurl); return NULL; }
    wcsncpy(szHost, urlComp.lpszHostName, urlComp.dwHostNameLength);
    szHost[urlComp.dwHostNameLength] = L'\0';

    char* result = NULL;
    HINTERNET hConnect = WinHttpConnect(hSession, szHost, urlComp.nPort, 0);
    if (hConnect) {
        HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", urlComp.lpszUrlPath,
                                               NULL, WINHTTP_NO_REFERER,
                                               WINHTTP_DEFAULT_ACCEPT_TYPES,
                                               (urlComp.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0);
        if (hRequest) {
            if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                   WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
                if (WinHttpReceiveResponse(hRequest, NULL)) {
                    char* response_headers = winhttp_response_headers(hRequest);
                    DWORD status_code = 0, status_size = sizeof(status_code);
                    if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                            WINHTTP_HEADER_NAME_BY_INDEX, &status_code, &status_size, WINHTTP_NO_HEADER_INDEX))
                        response_status = (int)status_code;
                    nal_monitor_set_last_response(response_headers ? response_headers : "", url, response_status);
                    char host_utf8[512] = {0};
                    (void)WideCharToMultiByte(CP_UTF8, 0, szHost, -1, host_utf8, (int)sizeof host_utf8, NULL, NULL);
                    websec_hsts_remember(host_utf8, urlComp.nScheme == INTERNET_SCHEME_HTTPS, response_headers);
                    if (response_headers && is_document) websec_set_response(url, response_headers);
                    free(response_headers);
                    DWORD dwSize = 0;
                    DWORD dwDownloaded = 0;
                    DWORD dwTotalSize = 0;
                    do {
                        dwSize = 0;
                        if (!WinHttpQueryDataAvailable(hRequest, &dwSize)) break;
                        if (dwSize == 0) break;

                        char* pszOutBuffer = realloc(result, dwTotalSize + dwSize + 1);
                        if (!pszOutBuffer) {
                            free(result);
                            result = NULL;
                            break;
                        }
                        result = pszOutBuffer;

                        if (WinHttpReadData(hRequest, (LPVOID)(result + dwTotalSize), dwSize, &dwDownloaded)) {
                            dwTotalSize += dwDownloaded;
                        }
                    } while (dwSize > 0);

                    if (result) result[dwTotalSize] = '\0';
                }
            }
            WinHttpCloseHandle(hRequest);
        }
        WinHttpCloseHandle(hConnect);
    }
    free(wurl);
    nal_monitor_record("GET", url, response_status, nal_monitor_now_ms() - started,
                       result ? (long long)strlen(result) : 0, is_document ? "document" : "resource");
    return result;
}

char* nal_http_get_kind(const char* url, int resource_kind) {
    if (!url) return NULL;
    char upgraded[4096];
    const char* effective = url;
    if (websec_hsts_upgrade_url(url, upgraded, sizeof upgraded)) effective = upgraded;
    int is_doc = websec_is_document_url(effective) && resource_kind == NAL_RESOURCE_OTHER;
    if (!is_doc && !nal_security_allows(effective, resource_kind)) {
        gal_log("WARN", "CSP: blocked resource %s (kind=%d)", effective, resource_kind);
        return NULL;
    }
    return nal_http_get_unchecked(effective);
}
char* nal_http_get(const char* url) { return nal_http_get_kind(url, NAL_RESOURCE_OTHER); }

static int nal_download_file_unchecked(const char* url, const char* dest_path) {
    gal_log("INFO", "NAL: downloading file (Win32): %s -> %s", url, dest_path);
    // Для простоты реализации на Windows можно использовать URLDownloadToFile из urlmon
    // Но мы уже начали с WinHTTP, так что лучше продолжить им для консистентности.
    // Однако WinHTTP требует больше кода для записи в файл.
    // В учебных целях/для быстрого фикса используем системный curl если есть,
    // или WinHTTP. Давай попробуем WinHTTP но с записью в файл.

    // (Упрощенно: получаем в память и пишем в файл, для картинок это норм если не гигабайты)
    // ВНИМАНИЕ: nal_http_get обрезает по \0 в конце, но для бинарных данных это НЕПРАВИЛЬНО.
    // Нам нужно было бы передавать размер.

    // Давай сделаем честный WinHTTP download для файлов.

    if (!hSession) return 0;

    URL_COMPONENTS urlComp;
    ZeroMemory(&urlComp, sizeof(urlComp));
    urlComp.dwStructSize = sizeof(urlComp);
    urlComp.dwHostNameLength = (DWORD)-1;
    urlComp.dwUrlPathLength = (DWORD)-1;
    urlComp.dwExtraInfoLength = (DWORD)-1;

    WCHAR* wurl = char_to_wchar(url);
    if (!WinHttpCrackUrl(wurl, (DWORD)wcslen(wurl), 0, &urlComp)) { free(wurl); return 0; }

    WCHAR szHost[256];
    if (urlComp.dwHostNameLength >= 256) { free(wurl); return 0; }
    wcsncpy(szHost, urlComp.lpszHostName, urlComp.dwHostNameLength);
    szHost[urlComp.dwHostNameLength] = L'\0';

    int success = 0;
    HINTERNET hConnect = WinHttpConnect(hSession, szHost, urlComp.nPort, 0);
    if (hConnect) {
        HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", urlComp.lpszUrlPath,
                                               NULL, WINHTTP_NO_REFERER,
                                               WINHTTP_DEFAULT_ACCEPT_TYPES,
                                               (urlComp.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0);
        if (hRequest) {
            if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                   WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
                if (WinHttpReceiveResponse(hRequest, NULL)) {
                    FILE* f = fopen(dest_path, "wb");
                    if (f) {
                        DWORD dwSize = 0;
                        DWORD dwDownloaded = 0;
                        BYTE buffer[8192];
                        do {
                            dwSize = 0;
                            if (!WinHttpQueryDataAvailable(hRequest, &dwSize)) break;
                            if (dwSize == 0) break;
                            DWORD toRead = (dwSize > sizeof(buffer)) ? sizeof(buffer) : dwSize;
                            if (WinHttpReadData(hRequest, (LPVOID)buffer, toRead, &dwDownloaded)) {
                                fwrite(buffer, 1, dwDownloaded, f);
                            }
                        } while (dwSize > 0);
                        fclose(f);
                        success = 1;
                    }
                }
            }
            WinHttpCloseHandle(hRequest);
        }
        WinHttpCloseHandle(hConnect);
    }
    free(wurl);
    return success;
}

int nal_download_file_kind(const char* url, const char* dest_path, int resource_kind) {
    if (!url || !dest_path) return 0;
    char upgraded[4096];
    const char* effective = url;
    if (websec_hsts_upgrade_url(url, upgraded, sizeof upgraded)) effective = upgraded;
    if (!nal_security_allows(effective, resource_kind)) {
        gal_log("WARN", "CSP: blocked download %s (kind=%d)", effective, resource_kind);
        return 0;
    }
    return nal_download_file_unchecked(effective, dest_path);
}
int nal_download_file(const char* url, const char* dest_path) { return nal_download_file_kind(url, dest_path, NAL_RESOURCE_OTHER); }

/* ── Асинхронные запросы: на Windows пока нет процесса-фона для сети (WinHTTP-версия синхронная),
   поэтому здесь honest-деградация — старт сразу же полностью выполняет запрос синхронно и помечает
   результат готовым к первому poll(). Хуже, чем настоящий async, но интерфейс для JS-слоя тот же,
   и при появлении WinHTTP-асинхронных хэндлов это можно заменить без изменений в js.c. */
typedef struct { char* body; char* headers; size_t body_len; int status; } WinAsyncResult;

int nal_async_start_ex(const char* method, const char* url, const char* headers_raw, const char* body, size_t body_len, const NalFetchOpts* opts, NalAsyncReq* out) {
    (void)opts;                                                          /* SOP/CORS на Windows пока не реализованы (WinHTTP-путь) */
    return nal_async_start(method, url, headers_raw, body, body_len, out);
}

int nal_async_start(const char* method, const char* url, const char* headers_raw, const char* body, size_t body_len, NalAsyncReq* out) {
    (void)headers_raw; (void)body; (void)body_len;
    memset(out, 0, sizeof *out);
    out->body_fd = -1;
    /* используем существующий синхронный путь как временную меру (методы кроме GET не поддержаны на Windows) */
    char* txt = (!method || !strcmp(method, "GET")) ? nal_http_get_kind(url, NAL_RESOURCE_CONNECT) : NULL;
    WinAsyncResult* r = malloc(sizeof *r);
    r->body = txt ? txt : strdup("");
    r->body_len = r->body ? strlen(r->body) : 0;
    r->headers = strdup(txt ? "HTTP/1.1 200 OK\r\n" : "HTTP/1.1 0 Network Error\r\n");
    r->status = txt ? 200 : 0;
    out->used = 1; out->pid = (pid_t)(intptr_t)r;                 /* переиспользуем поле как указатель на результат */
    return 1;
}
/* Windows пока использует синхронный WinHTTP-путь; интерфейс выдаёт тело чанками из памяти, но не обещает сетевое streaming. */
int nal_async_start_stream_ex(const char* method, const char* url, const char* headers_raw, const char* body, size_t body_len, const NalFetchOpts* opts, NalAsyncReq* out) {
    if (!nal_async_start_ex(method, url, headers_raw, body, body_len, opts, out)) return 0;
    out->streaming = 1; out->headers_sent = 0; out->body_offset = 0; out->finished = 1;
    WinAsyncResult* r = (WinAsyncResult*)(intptr_t)out->pid; out->exit_code = r && r->status ? 0 : 1;
    return 1;
}
int nal_async_stream_headers(NalAsyncReq* req, int* out_status, char** out_headers) {
    if (out_status) *out_status = 0; if (out_headers) *out_headers = NULL;
    if (!req || !req->used || !req->streaming || !out_status || !out_headers) return -1;
    WinAsyncResult* r = (WinAsyncResult*)(intptr_t)req->pid; if (!r) return -1;
    *out_status = r->status; *out_headers = _strdup(r->headers ? r->headers : ""); req->headers_sent = 1;
    return *out_headers ? (r->status ? 1 : -1) : -1;
}
int nal_async_stream_read(NalAsyncReq* req, unsigned char* buffer, size_t capacity, size_t* out_read, int* out_done, int* out_error) {
    if (out_read) *out_read = 0; if (out_done) *out_done = 0; if (out_error) *out_error = 0;
    if (!req || !req->used || !req->streaming || !buffer || !capacity || !out_read || !out_done || !out_error) return -1;
    WinAsyncResult* r = (WinAsyncResult*)(intptr_t)req->pid; if (!r) return -1;
    if (req->body_offset < r->body_len) { size_t n = r->body_len - req->body_offset; if (n > capacity) n = capacity; memcpy(buffer, r->body + req->body_offset, n); req->body_offset += n; *out_read = n; return 1; }
    *out_done = 1; *out_error = r->status == 0; return 1;
}
void nal_async_stream_close(NalAsyncReq* req, int cancel) {
    (void)cancel; if (!req || !req->used || !req->streaming) return;
    WinAsyncResult* r = (WinAsyncResult*)(intptr_t)req->pid;
    if (r) { free(r->body); free(r->headers); free(r); }
    req->used = 0; req->pid = 0;
}

int nal_async_poll(NalAsyncReq* req, int* out_status, char** out_headers, char** out_body) {
    if (!req->used) return 0;
    WinAsyncResult* r = (WinAsyncResult*)(intptr_t)req->pid;
    *out_status = r->status; *out_headers = r->headers; *out_body = r->body;
    free(r); req->used = 0; return 1;
}
int nal_async_poll_ex(NalAsyncReq* req, int* out_status, char** out_headers, char** out_body, size_t* out_body_len) {
    int r = nal_async_poll(req, out_status, out_headers, out_body);
    if (r && out_body_len) *out_body_len = *out_body ? strlen(*out_body) : 0;          /* WinHTTP-путь отдаёт текст */
    return r;
}

void nal_async_cancel(NalAsyncReq* req) {
    if (!req->used) return;
    if (req->streaming) { nal_async_stream_close(req, 1); return; }
    WinAsyncResult* r = (WinAsyncResult*)(intptr_t)req->pid;
    free(r->body); free(r->headers); free(r); req->used = 0;
}
