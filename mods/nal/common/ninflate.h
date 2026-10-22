#ifndef NINFLATE_H
#define NINFLATE_H
#include <stddef.h>

/* Собственная распаковка DEFLATE (RFC 1951) и контейнеров gzip (RFC 1952) / zlib (RFC 1950).
   Все функции возвращают malloc'ированный буфер (+ завершающий нулевой байт, не входящий в *out_len)
   или NULL при ошибке/превышении max_out. max_out == 0 — без ограничения. */
unsigned char* n_gunzip(const unsigned char* in, size_t in_len, size_t* out_len, size_t max_out);
unsigned char* n_zlib_decode(const unsigned char* in, size_t in_len, size_t* out_len, size_t max_out);
unsigned char* n_deflate_raw(const unsigned char* in, size_t in_len, size_t* out_len, size_t max_out);
/* «deflate» из Content-Encoding: по RFC это zlib-поток, но часть серверов шлёт «сырой» DEFLATE — определяем по заголовку */
unsigned char* n_http_deflate(const unsigned char* in, size_t in_len, size_t* out_len, size_t max_out);
#endif
