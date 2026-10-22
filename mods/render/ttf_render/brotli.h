#ifndef BROTLI_DEC_H
#define BROTLI_DEC_H
#include <stddef.h>
#include <stdint.h>

/* Собственный декодер Brotli (RFC 7932): нужен для WOFF2. Словарь и таблицы преобразований встроены (brotli_tables.inc).
   Возвращает malloc'ированный буфер (с запасным нулевым байтом после данных) или NULL при ошибке/превышении max_out (0 — без ограничения).
   size_hint — ожидаемый размер результата (ускоряет выделение памяти), может быть 0. */
uint8_t* brotli_decode(const uint8_t* in, size_t in_len, size_t size_hint, size_t max_out, size_t* out_len);
#endif
