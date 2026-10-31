#ifndef TTF_RENDER_H
#define TTF_RENDER_H

#include <stdint.h>
#include <stddef.h>

/*
 * TrueType рендерер (glyf-контуры, TTF/TTC/WOFF1, в т.ч. variable-шрифты
 * в их дефолтном инстансе).
 *
 * Что умеет:
 *   - точное покрытие пикселя по площади (настоящее сглаживание, без ступенек);
 *   - составные глифы (й, ё, Ä, é ... — база + диакритика);
 *   - дробные advance'ы и субпиксельное позиционирование (1/4 px по X);
 *   - кернинг из GPOS ('kern' feature, PairPos fmt 1/2) и из legacy-таблицы 'kern';
 *   - fallback на другой шрифт для глифов, которых нет в текущем;
 *   - кэш глифов по (глиф, размер, субпиксель) без обращений к диску.
 *
 * Координата y в ttf_render_string — верх строки; базовая линия лежит на
 * y + ascent(hhea) * size / units_per_em (округлено до целого пикселя).
 */

/* Один растеризованный глиф (альфа-маска 8 бит). left/top — смещение
   левого верхнего угла маски от origin/baseline (top — вверх от baseline). */
typedef struct {
    int16_t  left, top;
    uint16_t w, h;
    uint8_t* px;
} TTFGlyphBmp;

typedef struct {
    uint32_t    key;      /* ((gid << 2) | subpixel) + 1, 0 = пустой слот */
    TTFGlyphBmp bmp;
} TTFCacheSlot;

/* Кэш глифов одного размера шрифта (открытая адресация). */
typedef struct {
    int           size;
    uint32_t      stamp;
    uint32_t      cap, count;
    TTFCacheSlot* slots;
} TTFStrike;

typedef struct { uint32_t off; uint8_t lookup; } TTFKernSub;
typedef struct { uint32_t key; int16_t val; } TTFKernEntry;

#define TTF_NUM_STRIKES    8
#define TTF_BOLD           1
#define TTF_ITALIC         2
#define TTF_MAX_KERN_SUBS  128
#define TTF_KERN_CACHE     1024

/* Загруженный шрифт. Публичные поля: filepath, units_per_em, ascent,
   descent, line_gap. Остальное — внутренности рендерера. */
typedef struct TTFFont {
    char     filepath[512];
    int16_t  units_per_em;
    int16_t  ascent;          /* hhea, font units */
    int16_t  descent;         /* hhea, font units (обычно < 0) */
    int16_t  line_gap;

    /* --- private --- */
    uint8_t* data;            /* весь файл в памяти */
    size_t   data_len;
    uint32_t cmap_sub;        /* смещение выбранной cmap-подтаблицы */
    int      cmap_fmt;        /* 4 или 12 */
    uint32_t loca_off, loca_len, glyf_off, glyf_len;
    uint32_t hmtx_off, hmtx_len;
    uint32_t gpos_off, kern_off;
    struct CffFont* cff;      /* OpenType с PostScript-контурами («CFF »); для TrueType-контуров NULL */
    int      loca_format;
    int      num_glyphs;
    int      num_hmetrics;
    uint16_t ascii_gid[128];
    uint32_t cp_key[512];     /* прямой кэш кодпоинт -> глиф (ключ = cp + 1) */
    uint16_t cp_gid[512];

    int        kern_nsubs;
    TTFKernSub kern_subs[TTF_MAX_KERN_SUBS];
    TTFKernEntry kern_cache[TTF_KERN_CACHE];

    TTFStrike strikes[TTF_NUM_STRIKES];
    uint32_t  clock;

    struct TTFFont* fallback; /* используется, если в этом шрифте нет глифа */
} TTFFont;

/* === API === */

/* Загрузить шрифт из файла (TTF, TTC — первый шрифт, WOFF1). NULL при ошибке. */
TTFFont* ttf_load(const char* path);

/* Загрузить из памяти. Буфер должен быть выделен через malloc; при успехе
   владение переходит шрифту, при ошибке буфер освобождается. */
TTFFont* ttf_load_memory(uint8_t* data, size_t len, const char* name);

/* Шрифт, в котором ищутся глифы, отсутствующие в font (может быть NULL). */
void ttf_set_fallback(TTFFont* font, TTFFont* fallback);

/* Есть ли в шрифте глиф для кодпоинта. */
int ttf_has_glyph(TTFFont* font, uint32_t codepoint);

/* Отрендерить строку (UTF-8) в буфер 0xRRGGBB. y — верх строки.
   Возвращает ширину строки в пикселях. */
int ttf_render_string(TTFFont* font, const char* text,
                      uint32_t* buffer, int buf_w, int buf_h,
                      int font_size, int x, int y, uint32_t color);

/* Ширина строки в пикселях (без рендеринга). Совпадает с тем, что вернёт
   ttf_render_string для той же строки. */
int ttf_text_width(TTFFont* font, const char* text, int font_size);

/* Ширина первых nbytes байт строки — для позиции курсора / выделения.
   Учитывает кернинг, поэтому ширина префикса согласована с целой строкой. */
int ttf_text_width_n(TTFFont* font, const char* text, size_t nbytes, int font_size);

/* Позиции глифов для выделения/курсора: out[i] — x-смещение (px) начала символа,
   которому принадлежит байт i; out[strlen(text)] — полная ширина. Нужно cap >= strlen+1.
   Прямоугольники [out[a], out[b]) стыкуются без щелей и учитывают кернинг.
   Возвращает полную ширину (0 при ошибке). */
int ttf_text_offsets(TTFFont* font, const char* text, int font_size, int* out, int cap);

/* Метрики шрифта в пикселях для данного размера. */
int ttf_font_ascent(TTFFont* font, int font_size);
int ttf_font_descent(TTFFont* font, int font_size);   /* положительное число */

/* Расширенный API: стиль (TTF_BOLD | TTF_ITALIC — синтетические), stride буфера и
   прямоугольник отсечения [cx0,cx1) x [cy0,cy1). Возвращает ширину строки. */
int   ttf_render_string_ex(TTFFont* font, const char* text, uint32_t* buffer, int stride,
                           int cx0, int cy0, int cx1, int cy1,
                           int font_size, int x, int y, uint32_t color, int style);
float ttf_text_width_f(TTFFont* font, const char* text, int font_size, int style);
int   ttf_text_offsets_ex(TTFFont* font, const char* text, int font_size, int style, int* out, int cap);
/* CSS bidi-aware counterparts. direction: -1=first-strong/default LTR, 0=ltr,
 * 1=rtl; bidi_override enables unicode-bidi:bidi-override behavior. The offsets
 * array contains logical caret positions (so offsets[n] can be 0 in RTL); the
 * function return value is the visual width. */
int   ttf_render_string_ex_bidi(TTFFont* font, const char* text, uint32_t* buffer, int stride,
                                int cx0, int cy0, int cx1, int cy1,
                                int font_size, int x, int y, uint32_t color, int style,
                                int direction, int bidi_override);
int   ttf_text_offsets_ex_bidi(TTFFont* font, const char* text, int font_size, int style,
                               int* out, int cap, int direction, int bidi_override);
/* Hit-test an x coordinate against logical caret positions. Returns the UTF-8 byte
 * offset nearest to x, snapped to a stable grapheme boundary when possible. */
int   ttf_text_hit_test_ex_bidi(TTFFont* font, const char* text, int font_size, int style,
                                float x, int direction, int bidi_override, size_t* out_byte_offset);

/* Собран ли рендерер с поддержкой WOFF1 (нужен zlib). */
int ttf_supports_woff(void);
int ttf_supports_woff2(void);          /* WOFF2: собственный Brotli + реконструкция glyf/loca/hmtx (woff2.c, brotli.c) */

/* Освободить шрифт. */
void ttf_free(TTFFont* font);

#endif
