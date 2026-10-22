#ifndef CFF_H
#define CFF_H
#include <stddef.h>
#include <stdint.h>

/* Разбор таблицы «CFF » (Compact Font Format, Adobe TN #5176) и интерпретатор charstring'ов Type 2 (TN #5177) —
   контуры глифов для OpenType-шрифтов с PostScript-контурами. Поддерживаются обычные и CID-шрифты (FDArray/FDSelect),
   локальные и глобальные подпрограммы, hint-операторы (пропускаются), flex-операторы. CFF2 (вариативные) — нет. */
typedef struct CffFont CffFont;
typedef struct {
    void (*move)(void* ud, float x, float y);                                  /* начало контура (предыдущий закрывается) */
    void (*line)(void* ud, float x, float y);
    void (*curve)(void* ud, float x1, float y1, float x2, float y2, float x3, float y3);     /* кубическая кривая от текущей точки */
    void (*close)(void* ud);
    void* ud;
} CffSink;

CffFont* cff_open(const uint8_t* data, size_t len);        /* data должна жить, пока жив CffFont; NULL — формат не поддерживается */
void     cff_close(CffFont* f);
int      cff_num_glyphs(const CffFont* f);
int      cff_glyph(const CffFont* f, int gid, const CffSink* sink);   /* 1 — разобран (пустой глиф тоже), 0 — ошибка */
#endif
