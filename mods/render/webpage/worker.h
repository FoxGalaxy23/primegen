#ifndef WEBPAGE_WORKER_H
#define WEBPAGE_WORKER_H
#include <sys/types.h>

/* Web Worker: полноценный отдельный процесс со своим JSRuntime/JSContext (fork, не поток — упрощает
   изоляцию памяти, движок и так активно использует fork в других местах). Сообщения ходят как JSON-строки
   через пайпы (честный «structured clone» не реализован — годится всё, что переживает JSON.stringify/parse;
   это стандартное практическое ограничение и для многих настоящих воркер-библиотек). Worker не имеет
   доступа к DOM/window — только self, postMessage/onmessage, console, setTimeout/setInterval, и простой
   синхронный fetch (см. worker.c) — блокировать МОЖНО, это отдельный процесс, на UI не влияет. */
typedef struct {
    int used;
    pid_t pid;
    int rfd;   /* дочерний -> родитель: события (неблокирующий у родителя) */
    int wfd;   /* родитель -> дочерний: команды */
} WorkerConn;

int worker_start(const char* script_url, WorkerConn* out);

/* event: 0 нет событий; при возврате 1 заполняет *type: 'M' — message (*text — JSON, malloc, free() за вызывающим),
   'E' — error (*text — сообщение об ошибке, malloc), 'X' — воркер завершился (self.close() или упал). */
int worker_poll(WorkerConn* c, char* type, char** text);
void worker_post(WorkerConn* c, const char* json);
void worker_terminate(WorkerConn* c);

#endif
