/* Картинки: путь в кэше, параллельная предзагрузка (в дочернем процессе загрузки страницы),
   декодирование (stb_image: PNG/JPEG/GIF/BMP) и масштабирование с кэшем. */
#define _GNU_SOURCE
#include "webpage.h"
#include "nal.h"
#include "gal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#ifndef _WIN32
#include <sys/wait.h>
#endif

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_PSD
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_NO_TGA
#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
#include "stb_image.h"
#ifdef HAVE_WEBP
#include <webp/decode.h>
#endif

static unsigned long long fnv(const char* s) { unsigned long long h = 1469598103934665603ULL; for (; *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ULL; } return h; }

/* stb_image не декодирует WebP; большинство сайтов (в т.ч. Wikipedia) отдают им почти все картинки.
   Раз сигнатура RIFF....WEBP — декодируем через libwebp, возвращаем RGBA-буфер того же вида, что и stbi_load. */
static int is_webp(const unsigned char* m, size_t n) { return n >= 12 && !memcmp(m, "RIFF", 4) && !memcmp(m + 8, "WEBP", 4); }
#ifdef HAVE_WEBP
static unsigned char* load_webp(const char* path, int* w, int* h) {
    FILE* f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 64 * 1024 * 1024) { fclose(f); return NULL; }
    unsigned char* buf = malloc((size_t)sz); if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)sz, f); fclose(f);
    if (rd != (size_t)sz) { free(buf); return NULL; }
    unsigned char* out = WebPDecodeRGBA(buf, (size_t)sz, w, h);
    free(buf); return out;                                       /* тот же порядок байт RGBA, что и у stb_image */
}
#endif

int webpage_image_path(const char* url, char* out, size_t cap) {
    if (!url || !*url) return 0;
    if (strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0) { snprintf(out, cap, "cache/img_%016llx.bin", fnv(url)); return 1; }
    if (strncmp(url, "data:", 5) == 0) return 0;
    snprintf(out, cap, "%s", url); return 1;                                   /* локальный файл */
}

static int looks_like_image(const char* path) {
    FILE* f = fopen(path, "rb"); if (!f) return 0;
    unsigned char m[12] = {0}; size_t n = fread(m, 1, 12, f); fclose(f);
    if (n < 4) return 0;
    return (m[0] == 0x89 && m[1] == 'P') || (m[0] == 0xFF && m[1] == 0xD8) || !memcmp(m, "GIF8", 4) || (m[0] == 'B' && m[1] == 'M') || is_webp(m, n);
}

void webpage_prefetch_images(DOMNode* head) {
#ifndef _WIN32
    char* urls[48]; int nu = 0; struct stat st;
    mkdir("cache", 0777);
    for (DOMNode* n = head; n && nu < 48; n = n->next) {
        for (int k = 0; k < 2 && nu < 48; k++) {
            const char* u = k == 0 ? (n->type == TAG_IMG ? n->src : "") : n->st.bg_url;
            if (!u[0] || strncmp(u, "http", 4) != 0) continue;
            if (n->type != TAG_TEXT && n->st.display == DSP_NONE) continue;
            char p[600]; webpage_image_path(u, p, sizeof p);
            if (stat(p, &st) == 0 && st.st_size > 0) continue;
            int dup = 0; for (int i = 0; i < nu; i++) if (!strcmp(urls[i], u)) { dup = 1; break; }
            if (!dup) urls[nu++] = strdup(u);
        }
    }
    pid_t pids[6]; int np = 0;
    for (int i = 0; i < nu; i++) {
        pid_t pid = fork();
        if (pid == 0) { char p[600]; webpage_image_path(urls[i], p, sizeof p); if (!nal_download_file(urls[i], p) || !looks_like_image(p)) remove(p); _exit(0); }
        if (pid > 0) pids[np++] = pid;
        if (np == 6 || (i == nu - 1 && np)) { for (int k = 0; k < np; k++) waitpid(pids[k], NULL, 0); np = 0; }
    }
    for (int i = 0; i < nu; i++) free(urls[i]);
#else
    (void)head;
#endif
}

/* ══════════════ кэш декодированных картинок ══════════════ */
#define IC_MAX 96
typedef struct { char url[512]; ImgPix pix; int w2, h2; int fail; unsigned stamp; } ICEntry;   /* w2,h2 > 0 — масштабированная копия */
static ICEntry g_ic[IC_MAX]; static unsigned g_stamp = 0; static size_t g_bytes = 0;

static void ic_drop(ICEntry* e) { if (e->pix.px) { g_bytes -= (size_t)e->pix.w * e->pix.h * 4; free(e->pix.px); } memset(e, 0, sizeof *e); }
static ICEntry* ic_slot(void) {
    ICEntry* v = &g_ic[0];
    for (int i = 0; i < IC_MAX; i++) { if (!g_ic[i].url[0]) return &g_ic[i]; if (g_ic[i].stamp < v->stamp) v = &g_ic[i]; }
    ic_drop(v); return v;
}
static void ic_trim(void) {
    while (g_bytes > 192u * 1024 * 1024) {                                    /* держим кэш в разумных пределах */
        ICEntry* v = NULL; for (int i = 0; i < IC_MAX; i++) if (g_ic[i].pix.px && (!v || g_ic[i].stamp < v->stamp)) v = &g_ic[i];
        if (!v) break; ic_drop(v);
    }
}

ImgPix* webpage_image_get(const char* url) {
    if (!url || !*url) return NULL;
    for (int i = 0; i < IC_MAX; i++) if (!g_ic[i].w2 && g_ic[i].url[0] && !strcmp(g_ic[i].url, url)) { g_ic[i].stamp = ++g_stamp; return g_ic[i].fail ? NULL : &g_ic[i].pix; }
    ICEntry* e = ic_slot(); memset(e, 0, sizeof *e);
    snprintf(e->url, sizeof e->url, "%s", url); e->stamp = ++g_stamp;
    char path[600]; int w = 0, h = 0, n; unsigned char* d = NULL; int is_wp = 0;
    if (webpage_image_path(url, path, sizeof path)) {
        FILE* pf = fopen(path, "rb");
        if (pf) { unsigned char m[12] = {0}; size_t rn = fread(m, 1, 12, pf); fclose(pf); is_wp = is_webp(m, rn); }
#ifdef HAVE_WEBP
        if (is_wp) d = load_webp(path, &w, &h);
        else
#endif
        d = stbi_load(path, &w, &h, &n, 4);
    }
    if (!d || (long)w * h > 40000000L) { if (d) { if (is_wp) free(d); else stbi_image_free(d); } e->fail = 1; return NULL; }
    uint32_t* px = malloc((size_t)w * h * 4);
    if (!px) { if (is_wp) free(d); else stbi_image_free(d); e->fail = 1; return NULL; }
    for (long i = 0; i < (long)w * h; i++) px[i] = ((uint32_t)d[i * 4 + 3] << 24) | ((uint32_t)d[i * 4] << 16) | ((uint32_t)d[i * 4 + 1] << 8) | d[i * 4 + 2];
    if (is_wp) free(d); else stbi_image_free(d);
    e->pix.px = px; e->pix.w = w; e->pix.h = h; g_bytes += (size_t)w * h * 4; ic_trim();
    return &e->pix;
}

/* Инъекция «картинки» из другого источника пикселей (canvas 2D — см. js.c d_canvasCommit): подменяет/создаёт
   запись кэша по синтетическому ключу (например "canvas:37"), минуя файловую загрузку. Заодно сбрасывает
   все смасштабированные копии по этому ключу — после перерисовки канваса старые масштабированные версии
   уже не соответствуют содержимому. */
void webpage_image_inject(const char* key, const uint32_t* argb, int w, int h) {
    if (!key || !*key || w < 1 || h < 1) return;
    for (int i = 0; i < IC_MAX; i++) if (g_ic[i].w2 && g_ic[i].url[0] && !strcmp(g_ic[i].url, key)) ic_drop(&g_ic[i]);
    ICEntry* e = NULL;
    for (int i = 0; i < IC_MAX; i++) if (!g_ic[i].w2 && g_ic[i].url[0] && !strcmp(g_ic[i].url, key)) { e = &g_ic[i]; break; }
    if (!e) { e = ic_slot(); memset(e, 0, sizeof *e); snprintf(e->url, sizeof e->url, "%s", key); }
    else if (e->pix.px) { g_bytes -= (size_t)e->pix.w * e->pix.h * 4; free(e->pix.px); e->pix.px = NULL; }
    uint32_t* px = malloc((size_t)w * h * 4); if (!px) { e->fail = 1; return; }
    memcpy(px, argb, (size_t)w * h * 4);
    e->pix.px = px; e->pix.w = w; e->pix.h = h; e->fail = 0; e->stamp = ++g_stamp; g_bytes += (size_t)w * h * 4; ic_trim();
}

ImgPix* webpage_image_scaled(const char* url, int w, int h) {
    if (w < 1 || h < 1 || w > 8192 || h > 8192) return NULL;
    for (int i = 0; i < IC_MAX; i++) if (g_ic[i].w2 == w && g_ic[i].h2 == h && !strcmp(g_ic[i].url, url)) { g_ic[i].stamp = ++g_stamp; return &g_ic[i].pix; }
    ImgPix* src = webpage_image_get(url); if (!src) return NULL;
    if (src->w == w && src->h == h) return src;
    uint32_t* out = malloc((size_t)w * h * 4); if (!out) return NULL;
    for (int y = 0; y < h; y++) {
        float sy0 = (float)y * src->h / h, sy1 = (float)(y + 1) * src->h / h;
        for (int x = 0; x < w; x++) {
            float sx0 = (float)x * src->w / w, sx1 = (float)(x + 1) * src->w / w;
            int ix0 = (int)sx0, ix1 = (int)(sx1 + 0.999f), iy0 = (int)sy0, iy1 = (int)(sy1 + 0.999f);
            if (ix1 > src->w) ix1 = src->w; if (iy1 > src->h) iy1 = src->h; if (ix1 <= ix0) ix1 = ix0 + 1; if (iy1 <= iy0) iy1 = iy0 + 1;
            if (ix1 > src->w) { ix1 = src->w; ix0 = ix1 - 1; } if (iy1 > src->h) { iy1 = src->h; iy0 = iy1 - 1; }
            float ar = 0, ag = 0, ab = 0, aa = 0, wsum = 0;
            for (int yy = iy0; yy < iy1; yy++) {
                float wy = (yy + 1 < sy1 ? yy + 1 : sy1) - (yy > sy0 ? yy : sy0); if (wy <= 0) wy = 0.001f;
                for (int xx = ix0; xx < ix1; xx++) {
                    float wx = (xx + 1 < sx1 ? xx + 1 : sx1) - (xx > sx0 ? xx : sx0); if (wx <= 0) wx = 0.001f;
                    uint32_t p = src->px[(size_t)yy * src->w + xx]; float wt = wx * wy, a = (p >> 24) * wt;
                    aa += a; ar += ((p >> 16) & 255) * a; ag += ((p >> 8) & 255) * a; ab += (p & 255) * a; wsum += wt;
                }
            }
            uint32_t A = aa > 0 ? (uint32_t)(aa / wsum + .5f) : 0;
            uint32_t R = aa > 0 ? (uint32_t)(ar / aa + .5f) : 0, G = aa > 0 ? (uint32_t)(ag / aa + .5f) : 0, B = aa > 0 ? (uint32_t)(ab / aa + .5f) : 0;
            out[(size_t)y * w + x] = (A << 24) | (R << 16) | (G << 8) | B;
        }
    }
    ICEntry* e = ic_slot(); memset(e, 0, sizeof *e);
    snprintf(e->url, sizeof e->url, "%s", url); e->w2 = w; e->h2 = h; e->stamp = ++g_stamp;
    e->pix.px = out; e->pix.w = w; e->pix.h = h; g_bytes += (size_t)w * h * 4; ic_trim();
    return &e->pix;
}

void webpage_image_cache_clear(void) { for (int i = 0; i < IC_MAX; i++) ic_drop(&g_ic[i]); }
