#ifndef NAL_H
#define NAL_H

#include <stddef.h>
#include <sys/types.h>

/* Network Abstraction Layer (NAL) */

// Инициализация сетевой подсистемы (особенно важно для Windows/Winsock)
int nal_init(void);

// Завершение работы с сетью
void nal_close(void);

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

/* Неблокирующая проверка готовности. Возвращает: 0 — ещё выполняется, 1 — завершён (успешно или с сетевой ошибкой).
   При возврате 1 заполняет *out_status (0 при сетевой ошибке/обрыве), *out_headers и *out_body (malloc'ированные, может быть пустая строка;
   вызывающий обязан free()); также освобождает временные файлы. */
int nal_async_poll(NalAsyncReq* req, int* out_status, char** out_headers, char** out_body);

/* Отменяет ещё не завершённый запрос (SIGKILL дочернему процессу) и подчищает временные файлы. */
void nal_async_cancel(NalAsyncReq* req);

#endif
