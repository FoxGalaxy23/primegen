/* Выгрузка контуров CFF-глифов для проверки интерпретатора: cff_dump file.cff  (сырой блок таблицы «CFF »)
   Сборка: gcc -O2 -Imods/render/ttf_render tools/cff_dump.c mods/render/ttf_render/cff.c -lm -o /tmp/cff_dump */
#include "cff.h"
#include <stdio.h>
#include <stdlib.h>
static void mv(void* u, float x, float y) { (void)u; printf(" M %.3f %.3f", x, y); }
static void ln(void* u, float x, float y) { (void)u; printf(" L %.3f %.3f", x, y); }
static void cv(void* u, float a, float b, float c, float d, float e, float g) { (void)u; printf(" C %.3f %.3f %.3f %.3f %.3f %.3f", a, b, c, d, e, g); }
static void cl(void* u) { (void)u; printf(" Z"); }
int main(int argc, char** argv) {
    FILE* f = fopen(argv[1], "rb"); if (!f) return 2; fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* b = malloc((size_t)n); if (fread(b, 1, (size_t)n, f) != (size_t)n) return 2; fclose(f);
    CffFont* c = cff_open(b, (size_t)n); if (!c) { fprintf(stderr, "cff_open failed\n"); return 1; }
    CffSink s = {mv, ln, cv, cl, NULL}; int ng = cff_num_glyphs(c); int bad = 0;
    for (int g = 0; g < ng; g++) { printf("g%d:", g); if (!cff_glyph(c, g, &s)) { printf(" ERR"); bad++; } printf("\n"); }
    fprintf(stderr, "glyphs=%d errors=%d\n", ng, bad); return 0;
}
