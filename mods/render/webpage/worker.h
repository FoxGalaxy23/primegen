#ifndef WEBPAGE_WORKER_H
#define WEBPAGE_WORKER_H
#include <sys/types.h>
#include <stddef.h>
#include <stdint.h>

/* Web Worker transport. Messages are QuickJS structured-clone byte streams (not JSON).
   Transfer currently supports ArrayBuffer; SharedArrayBuffer/MessagePort transfer are not implemented. */
typedef struct {
    int used;
    pid_t pid;
    int rfd;   /* child -> parent framed events */
    int wfd;   /* parent -> child framed commands */
} WorkerConn;

int worker_start(const char* script_url, WorkerConn* out);
/* Returns 1 when an event is available. For M, *data is a malloc()ed QuickJS object stream;
   for E, it is a NUL-terminated error string. Caller frees *data. */
int worker_poll(WorkerConn* c, char* type, unsigned char** data, size_t* len);
int worker_post(WorkerConn* c, const unsigned char* data, size_t len);
void worker_terminate(WorkerConn* c);

#endif
