/* Распаковка Brotli для проверки декодера: brotli_cli in.br out.bin
   Сборка: gcc -O2 -Imods/render/ttf_render tools/brotli_cli.c mods/render/ttf_render/brotli.c -o /tmp/brotli_cli */
#include "brotli.h"
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char** argv) {
    if (argc < 3) return 2;
    FILE* f = fopen(argv[1], "rb"); if (!f) return 2; fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* b = malloc((size_t)n + 1); if (fread(b, 1, (size_t)n, f) != (size_t)n) return 2; fclose(f);
    size_t ol = 0; uint8_t* o = brotli_decode(b, (size_t)n, 0, 0, &ol); if (!o) { fprintf(stderr, "decode error\n"); return 1; }
    FILE* g = fopen(argv[2], "wb"); fwrite(o, 1, ol, g); fclose(g); return 0;
}
