#ifndef WOFF2_H
#define WOFF2_H
#include <stddef.h>
#include <stdint.h>
/* Конвертация WOFF2 в обычный sfnt (TrueType/OpenType): распаковка Brotli, обратное преобразование таблиц glyf/loca/hmtx,
   сборка таблицы каталога с контрольными суммами. Возвращает malloc'ированный буфер или NULL (в т.ч. для коллекций шрифтов). */
uint8_t* woff2_to_sfnt(const uint8_t* in, size_t in_len, size_t* out_len);
#endif
