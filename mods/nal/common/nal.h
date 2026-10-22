#ifndef NAL_H
#define NAL_H

#include <stddef.h>
#include <sys/types.h>

/* Network Abstraction Layer (NAL) */

// Инициализация сетевой подсистемы (особенно важно для Windows/Winsock)
int nal_init(void);

// Завершение работы с сетью
void nal_close(void);

/* HTTP-кэш (Linux: собственный клиент, см. mods/nal/README.md). Политика действует на запросы, начатые после вызова
   (процессы загрузки наследуют её при fork). На других платформах функции — заглушки. */
enum { NAL_CACHE_NORMAL = 0, NAL_CACHE_RELOAD = 1, NAL_CACHE_OFFLINE = 2 };   /* RELOAD — не доверять кэшу (кнопка «Обновить»), OFFLINE — только кэш */
void nal_cache_policy(int policy);
void nal_cache_clear(void);

// Загрузка содержимого по URL в память. Возвращает malloc'ированную строку или NULL.
char* nal_http_get(const char* url);

// Загрузка файла по URL и сохранение по пути dest_path.
// Возвращает 1 при успехе, 0 при ошибке.
int nal_download_file(const char* url, const char* dest_path);

/* ── Асинхронные запросы (fetch/XHR любых методов) ──
   Не блокируют вызывающий процесс: запрос выполняется в отдельном процессе (fork+exec curl),
   готовность проверяется неблокирующим nal_async_poll() из основного цикла событий. */
typedef struct {
    int used;
    pid_t pid;
    char body_path[128], hdr_path[128];
} NalAsyncReq;

/* method: "GET"/"POST"/... ; headers_raw: строки "Ключ: значение", разделённые '\n', может быть NULL;
   body/body_len: тело запроса (может быть NULL/0). Возвращает 1 и заполняет *out при успешном запуске, иначе 0. */
int nal_async_start(const char* method, const char* url, const char* headers_raw, const char* body, size_t body_len, NalAsyncReq* out);

/* Запрос из скрипта страницы с политикой одного источника и CORS (Linux: mods/nal/common/ncors.c). page_url == NULL — доверенный запрос
   браузера без проверок. credentials: 0 omit / 1 same-origin / 2 include; mode: 0 cors / 1 no-cors / 2 same-origin. Нарушение политики —
   как сетевая ошибка (status 0). На платформах без реализации действует прежнее поведение nal_async_start. */
typedef struct { const char* page_url; int credentials; int mode; } NalFetchOpts;

/* Смешанное содержимое: страница по https не должна подгружать «активные» ресурсы (скрипты, стили, шрифты, fetch) по http.
   Исключение — loopback (localhost, 127.x, ::1): разработка. 1 — загрузка должна быть запрещена. */
#include <strings.h>
#include <string.h>
static inline int nal_mixed_content_blocked(const char* page_url, const char* url) {
    if (!page_url || !url || strncasecmp(page_url, "https://", 8) != 0 || strncasecmp(url, "http://", 7) != 0) return 0;
    const char* h = url + 7;
    if (!strncasecmp(h, "localhost", 9) || !strncmp(h, "127.", 4) || !strncmp(h, "[::1]", 5)) return 0;
    return 1;
}
int nal_async_start_ex(const char* method, const char* url, const char* headers_raw, const char* body, size_t body_len, const NalFetchOpts* opts, NalAsyncReq* out);

/* Неблокирующая проверка готовности. Возвращает: 0 — ещё выполняется, 1 — завершён (успешно или с сетевой ошибкой).
   При возврате 1 заполняет *out_status (0 при сетевой ошибке/обрыве), *out_headers и *out_body (malloc'ированные, может быть пустая строка;
   вызывающий обязан free()); также освобождает временные файлы. */
int nal_async_poll(NalAsyncReq* req, int* out_status, char** out_headers, char** out_body);

/* Отменяет ещё не завершённый запрос (SIGKILL дочернему процессу) и подчищает временные файлы. */
void nal_async_cancel(NalAsyncReq* req);

#endif
