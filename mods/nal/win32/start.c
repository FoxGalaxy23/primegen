#include "nal.h"
#include "gal.h"
#include <windows.h>
#include <winhttp.h>
#include <stdio.h>

#pragma comment(lib, "winhttp.lib")

static HINTERNET hSession = NULL;

int nal_init(void) {
    hSession = WinHttpOpen(L"Meetlookers Browser/1.0",
                           WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                           WINHTTP_NO_PROXY_NAME,
                           WINHTTP_NO_PROXY_BYPASS, 0);
    return (hSession != NULL);
}

void nal_close(void) {
    if (hSession) {
        WinHttpCloseHandle(hSession);
        hSession = NULL;
    }
}

static WCHAR* char_to_wchar(const char* s) {
    int len = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    WCHAR* ws = malloc(len * sizeof(WCHAR));
    MultiByteToWideChar(CP_UTF8, 0, s, -1, ws, len);
    return ws;
}

char* nal_http_get(const char* url) {
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
    return result;
}

int nal_download_file(const char* url, const char* dest_path) {
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

/* ── Асинхронные запросы: на Windows пока нет процесса-фона для сети (WinHTTP-версия синхронная),
   поэтому здесь honest-деградация — старт сразу же полностью выполняет запрос синхронно и помечает
   результат готовым к первому poll(). Хуже, чем настоящий async, но интерфейс для JS-слоя тот же,
   и при появлении WinHTTP-асинхронных хэндлов это можно заменить без изменений в js.c. */
typedef struct { char* body; char* headers; int status; } WinAsyncResult;

int nal_async_start(const char* method, const char* url, const char* headers_raw, const char* body, size_t body_len, NalAsyncReq* out) {
    (void)headers_raw; (void)body; (void)body_len;
    memset(out, 0, sizeof *out);
    /* используем существующий синхронный путь как временную меру (методы кроме GET не поддержаны на Windows) */
    char* txt = (!method || !strcmp(method, "GET")) ? nal_http_get(url) : NULL;
    WinAsyncResult* r = malloc(sizeof *r);
    r->body = txt ? txt : strdup("");
    r->headers = strdup("HTTP/1.1 200 OK\r\n");
    r->status = txt ? 200 : 0;
    out->used = 1; out->pid = (pid_t)(intptr_t)r;                 /* переиспользуем поле как указатель на результат */
    return 1;
}
int nal_async_poll(NalAsyncReq* req, int* out_status, char** out_headers, char** out_body) {
    if (!req->used) return 0;
    WinAsyncResult* r = (WinAsyncResult*)(intptr_t)req->pid;
    *out_status = r->status; *out_headers = r->headers; *out_body = r->body;
    free(r); req->used = 0; return 1;
}
void nal_async_cancel(NalAsyncReq* req) {
    if (!req->used) return;
    WinAsyncResult* r = (WinAsyncResult*)(intptr_t)req->pid;
    free(r->body); free(r->headers); free(r); req->used = 0;
}
