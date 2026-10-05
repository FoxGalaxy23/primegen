#ifndef WEBPAGE_WS_H
#define WEBPAGE_WS_H
#include <sys/types.h>

/* Минимальный WebSocket-клиент: только ws:// (без TLS — wss:// возвращает событие ошибки).
   Соединение и цикл чтения/записи кадров живут в отдельном процессе (fork), поэтому основной процесс
   никогда не блокируется на сетевом I/O; события забираются неблокирующим ws_poll() из тика движка. */
typedef struct {
    int used;
    pid_t pid;
    int rfd;   /* дочерний -> родитель: события (неблокирующий на стороне родителя) */
    int wfd;   /* родитель -> дочерний: команды (send/close) */
} WsConn;

/* Запускает соединение в фоне. Возвращает 1 если процесс запущен (это НЕ значит, что handshake уже
   прошёл — за этим следить через ws_poll: придёт событие 'O' при успехе или 'E' при ошибке/wss). */
int ws_start(const char* url, WsConn* out);

/* Забирает одно готовое событие. Возвращает 0 если пока ничего нет. При возврате 1 заполняет *type:
   'O' (open, *text/*code не используются), 'M' (message, *text — malloc'ированная строка, вызывающий обязан free()),
   'C' (close, *code — код закрытия), 'E' (error). После 'C' или 'E' соединение считается завершённым. */
int ws_poll(WsConn* c, char* type, char** text, int* code);

void ws_send(WsConn* c, const char* text);
void ws_close(WsConn* c, int code);

#endif
