/* WOFF2 → TTF для проверки конвертера: woff2_cli in.woff2 out.ttf
   Сборка: gcc -O2 -Imods/render/ttf_render tools/woff2_cli.c mods/render/ttf_render/woff2.c mods/render/ttf_render/brotli.c -o /tmp/woff2_cli */
#include "woff2.h"
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char** argv) {
    if (argc < 3) return 2;
    FILE* f = fopen(argv[1], "rb"); if (!f) return 2; fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* b = malloc((size_t)n + 1); if (fread(b, 1, (size_t)n, f) != (size_t)n) return 2; fclose(f);
    size_t ol = 0; uint8_t* o = woff2_to_sfnt(b, (size_t)n, &ol); if (!o) { fprintf(stderr, "convert error\n"); return 1; }
    FILE* g = fopen(argv[2], "wb"); fwrite(o, 1, ol, g); fclose(g); return 0;
}
