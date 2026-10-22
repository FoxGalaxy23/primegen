#ifndef SVG_H
#define SVG_H
#include <stdint.h>
#include <stddef.h>

/* Растеризатор SVG (подмножество SVG 1.1): path/rect/circle/ellipse/line/polyline/polygon, g/a/switch, use/symbol/defs,
   вложенный svg с viewBox и preserveAspectRatio, transform, fill/stroke (цвета, currentColor, linearGradient/radialGradient),
   fill-rule, opacity, stroke-width/linecap/linejoin/miterlimit/dasharray, style="..." и <style> с простыми селекторами.
   Не поддерживается: text, image, filter, mask, clip-path (игнорируются), pattern, marker, анимации. */

/* Похоже ли содержимое буфера на SVG (для распознавания файлов, скачанных без расширения) */
int  svg_looks_like(const char* buf, size_t n);
/* Собственный размер по корневому <svg> (width/height, иначе viewBox, иначе 300x150). 1 — разобрано */
int  svg_intrinsic(const char* src, size_t len, float* w, float* h);
/* Растеризация в w×h пикселей: 0xAARRGGBB без предумножения, malloc; NULL при ошибке. cur_color — значение currentColor */
uint32_t* svg_render(const char* src, size_t len, int w, int h, uint32_t cur_color);
#endif
