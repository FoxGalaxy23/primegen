/* Проверка растеризатора SVG без браузера.
   Сборка: gcc -O2 -Imods/render/webpage tools/svg_cli.c mods/render/webpage/svg.c -lm -o /tmp/svg_cli
   Запуск: /tmp/svg_cli in.svg out.ppm WIDTH HEIGHT [#rrggbb цвет currentColor] — результат рисуется на белом фоне */
#include "svg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char** argv) {
    if (argc < 5) { fprintf(stderr, "usage: svg_cli in.svg out.ppm W H [color]\n"); return 2; }
    FILE* f = fopen(argv[1], "rb"); if (!f) { perror("open"); return 1; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET); char* buf = malloc((size_t)n + 1); if (fread(buf, 1, (size_t)n, f) != (size_t)n) return 1; buf[n] = 0; fclose(f);
    int W = atoi(argv[3]), H = atoi(argv[4]); uint32_t cc = 0xFF000000u; if (argc > 5 && argv[5][0] == '#') cc = 0xFF000000u | (uint32_t)strtoul(argv[5] + 1, NULL, 16);
    float iw = 0, ih = 0; if (svg_intrinsic(buf, (size_t)n, &iw, &ih)) fprintf(stderr, "intrinsic: %.1f x %.1f\n", iw, ih);
    uint32_t* px = svg_render(buf, (size_t)n, W, H, cc); if (!px) { fprintf(stderr, "render failed\n"); return 1; }
    FILE* o = fopen(argv[2], "wb"); fprintf(o, "P6\n%d %d\n255\n", W, H);
    for (long i = 0; i < (long)W * H; i++) { uint32_t p = px[i]; unsigned a = p >> 24, r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
        unsigned char c[3] = {(unsigned char)((r * a + 255 * (255 - a)) / 255), (unsigned char)((g * a + 255 * (255 - a)) / 255), (unsigned char)((b * a + 255 * (255 - a)) / 255)}; fwrite(c, 1, 3, o); }
    fclose(o); free(px); free(buf); return 0;
}
