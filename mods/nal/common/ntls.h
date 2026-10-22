#ifndef NTLS_H
#define NTLS_H
#include <stddef.h>

/* Интерфейс TLS-клиента для собственного HTTP-стека (nhttp.c). Реализация подменяемая: сейчас — ntls_openssl.c
   (системный OpenSSL через dlopen, без заголовков на этапе сборки); сюда же можно подключить собственную
   реализацию TLS 1.2/1.3, не трогая HTTP-слой. Сокет — блокирующий, с SO_RCVTIMEO/SO_SNDTIMEO. */
typedef struct NTls NTls;

int         ntls_available(void);                  /* 1 — TLS-библиотека найдена и инициализирована */
const char* ntls_backend_name(void);
/* Рукопожатие поверх готового соединённого сокета fd: SNI, проверка цепочки сертификатов по системному
   хранилищу CA и проверка имени хоста (или IP). Возвращает NULL и текст ошибки в err при неудаче. */
NTls* ntls_connect(int fd, const char* host, char* err, size_t errcap);
/* >0 — число прочитанных байт; 0 — конец потока; -1 — ошибка; -2 — таймаут сокета */
int   ntls_read(NTls* t, void* buf, size_t n);
/* n — записано всё; -1 — ошибка; -2 — таймаут */
int   ntls_write(NTls* t, const void* buf, size_t n);
/* send_shutdown=0 — не слать close_notify (соединение унаследовано от родителя после fork и принадлежит ему); fd закрывает вызывающий */
void  ntls_close(NTls* t, int send_shutdown);
#endif
