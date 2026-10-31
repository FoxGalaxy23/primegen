/* Картинки: путь в кэше, параллельная предзагрузка, декодирование stb_image +
   optional libwebp/libavif и масштабирование с кэшем. */
#define _GNU_SOURCE
#include "webpage.h"
#include "nal.h"
#include "svg.h"
#include "gal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
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
#ifdef HAVE_AVIF
#include <avif/avif.h>
#endif

static unsigned long long fnv(const char* s) { unsigned long long h = 1469598103934665603ULL; for (; *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ULL; } return h; }

/* stb_image не декодирует WebP; большинство сайтов (в т.ч. Wikipedia) отдают им почти все картинки.
   Раз сигнатура RIFF....WEBP — декодируем через libwebp, возвращаем RGBA-буфер того же вида, что и stbi_load. */
static int is_webp(const unsigned char* m, size_t n) { return n >= 12 && !memcmp(m, "RIFF", 4) && !memcmp(m + 8, "WEBP", 4); }
static int is_avif(const unsigned char* m, size_t n) {
    if (n < 16 || memcmp(m + 4, "ftyp", 4)) return 0;
    /* ISO BMFF major brand + compatible brands (e.g. avif/avis). */
    size_t lim = n < 80 ? n : 80;
    for (size_t i = 8; i + 4 <= lim; i += 4)
        if (!memcmp(m + i, "avif", 4) || !memcmp(m + i, "avis", 4)) return 1;
    return 0;
}
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

#ifdef HAVE_AVIF
/* Возвращает компактный RGBA8-буфер, совместимый с дальнейшим путём обработки stb_image. */
static unsigned char* load_avif(const char* path, int* w, int* h) {
    avifDecoder* decoder = avifDecoderCreate();
    if (!decoder) return NULL;
    decoder->maxThreads = 2;
    decoder->imageSizeLimit = 40000000u;
    decoder->imageDimensionLimit = 16384u;
    decoder->imageCountLimit = 16u;
    unsigned char* out = NULL;
    avifResult rc = avifDecoderSetSource(decoder, AVIF_DECODER_SOURCE_PRIMARY_ITEM);
    if (rc == AVIF_RESULT_OK) rc = avifDecoderSetIOFile(decoder, path);
    if (rc == AVIF_RESULT_OK) rc = avifDecoderParse(decoder);
    if (rc == AVIF_RESULT_OK) rc = avifDecoderNextImage(decoder);
    avifImage* image = decoder->image;
    if (rc == AVIF_RESULT_OK && image && image->width && image->height &&
        (uint64_t)image->width * (uint64_t)image->height <= 40000000ull) {
        avifRGBImage rgb;
        avifRGBImageSetDefaults(&rgb, image);
        rgb.format = AVIF_RGB_FORMAT_RGBA;
        rgb.depth = 8;
        rgb.maxThreads = 2;
        if (avifRGBImageAllocatePixels(&rgb) == AVIF_RESULT_OK &&
            avifImageYUVToRGB(image, &rgb) == AVIF_RESULT_OK) {
            size_t row = (size_t)image->width * 4;
            out = (unsigned char*)malloc(row * image->height);
            if (out) {
                for (uint32_t y = 0; y < image->height; ++y)
                    memcpy(out + (size_t)y * row, rgb.pixels + (size_t)y * rgb.rowBytes, row);
                *w = (int)image->width; *h = (int)image->height;
            }
        }
        avifRGBImageFreePixels(&rgb);
    }
    avifDecoderDestroy(decoder);
    return out;
}
#endif

#ifdef HAVE_WEBP
static void free_webp(unsigned char* p) { if (p) WebPFree(p); }
#endif
static void free_decoder_buffer(unsigned char* p, int kind) {
#ifdef HAVE_WEBP
    if (kind == 1) { free_webp(p); return; }
#else
    (void)kind;
#endif
    free(p);
}

int webpage_image_path(const char* url, char* out, size_t cap) {
    if (!url || !*url) return 0;
    if (strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0) { snprintf(out, cap, "cache/img_%016llx.bin", fnv(url)); return 1; }
    if (strncmp(url, "data:", 5) == 0) return webpage_data_uri_to_file(url, strlen(url), out, cap);
    snprintf(out, cap, "%s", url); return 1;                                   /* локальный файл */
}

static int looks_like_image(const char* path) {
    FILE* f = fopen(path, "rb"); if (!f) return 0;
    unsigned char m[4096] = {0}; size_t n = fread(m, 1, sizeof m, f); fclose(f);
    if (n < 4) return 0;
    return (m[0] == 0x89 && m[1] == 'P') || (m[0] == 0xFF && m[1] == 0xD8) || !memcmp(m, "GIF8", 4) || (m[0] == 'B' && m[1] == 'M') || is_webp(m, n) || is_avif(m, n) || svg_looks_like((const char*)m, n);
}

/* ── data: URI → файл кэша ──
   Поддерживаются «data:[<mime>][;base64],<данные>» (base64 и %-кодирование, а также «сырой» текст — типично для SVG).
   Длинные data-URI не помещаются в поля узлов (src[512], bg_url[192]), поэтому декодируем сразу в файл и дальше
   работаем с обычным локальным путём. */
static int b64v(int c) { if (c >= 'A' && c <= 'Z') return c - 'A'; if (c >= 'a' && c <= 'z') return c - 'a' + 26; if (c >= '0' && c <= '9') return c - '0' + 52; if (c == '+' || c == '-') return 62; if (c == '/' || c == '_') return 63; return -1; }
int webpage_data_uri_to_file(const char* uri, size_t len, char* out, size_t cap) {
    if (len < 6 || strncasecmp(uri, "data:", 5) != 0) return 0;
    const char* comma = memchr(uri, ',', len); if (!comma) return 0;
    size_t hl = (size_t)(comma - uri); int b64 = 0;
    for (size_t i = 5; i + 6 <= hl; i++) if (!strncasecmp(uri + i, "base64", 6)) { b64 = 1; break; }
    const char* d = comma + 1; size_t dl = len - hl - 1;
    unsigned long long h = 1469598103934665603ULL; for (size_t i = 0; i < len; i++) { h ^= (unsigned char)uri[i]; h *= 1099511628211ULL; }
    snprintf(out, cap, "cache/data_%016llx.bin", h);
    struct stat st; if (stat(out, &st) == 0 && st.st_size > 0) return 1;
    unsigned char* buf = malloc(dl + 1); if (!buf) return 0; size_t o = 0;
    if (b64) { unsigned acc = 0; int bits = 0; for (size_t i = 0; i < dl; i++) { int v = b64v((unsigned char)d[i]); if (v < 0) { if (d[i] == '%' && i + 2 < dl) { /* %-кодированный base64 (+, /, =) */ } continue; } acc = (acc << 6) | (unsigned)v; bits += 6; if (bits >= 8) { bits -= 8; buf[o++] = (unsigned char)((acc >> bits) & 255); } } }
    else for (size_t i = 0; i < dl; i++) {
        if (d[i] == '%' && i + 2 < dl + 0 && isxdigit((unsigned char)d[i + 1]) && isxdigit((unsigned char)d[i + 2])) { char hx[3] = {d[i + 1], d[i + 2], 0}; buf[o++] = (unsigned char)strtol(hx, NULL, 16); i += 2; }
        else buf[o++] = (unsigned char)d[i];
    }
    mkdir("cache", 0777);
    FILE* f = fopen(out, "wb"); if (!f) { free(buf); return 0; }
    fwrite(buf, 1, o, f); fclose(f); free(buf); return o > 0;
}
static char* read_file_cap(const char* path, size_t cap, size_t* len) {
    FILE* f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0 || (size_t)sz > cap) { fclose(f); return NULL; }
    char* b = malloc((size_t)sz + 1); if (!b) { fclose(f); return NULL; }
    size_t rd = fread(b, 1, (size_t)sz, f); fclose(f); b[rd] = 0; *len = rd; return b;
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
        if (pid == 0) { char p[600]; webpage_image_path(urls[i], p, sizeof p); if (!nal_download_file_kind(urls[i], p, NAL_RESOURCE_IMAGE) || !looks_like_image(p)) remove(p); _exit(0); }
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
typedef struct { char url[512]; ImgPix pix; int w2, h2; int fail; unsigned stamp; int svg; uint32_t cc; } ICEntry;   /* w2,h2 > 0 — масштабированная копия; svg — векторная картинка, cc — цвет currentColor, с которым она растеризована */
static uint32_t g_cc = 0xFF000000u;
/* Цвет для currentColor следующих запросов webpage_image_scaled (встроенный <svg> берёт его из CSS `color` элемента) */
void webpage_image_set_color(uint32_t argb) { g_cc = argb; }
/* длинные ключи (data-URI и т.п.) укорачиваем с хэшем, чтобы записи кэша не склеивались по общему префиксу */
static const char* ic_key(const char* url, char* buf, size_t cap) { if (strlen(url) < 500) return url; snprintf(buf, cap, "%.200s#%016llx", url, fnv(url)); return buf; }
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
    char kb[600]; const char* key = ic_key(url, kb, sizeof kb);
    for (int i = 0; i < IC_MAX; i++) if (!g_ic[i].w2 && g_ic[i].url[0] && !strcmp(g_ic[i].url, key)) { g_ic[i].stamp = ++g_stamp; return g_ic[i].fail ? NULL : &g_ic[i].pix; }
    ICEntry* e = ic_slot(); memset(e, 0, sizeof *e);
    snprintf(e->url, sizeof e->url, "%s", key); e->stamp = ++g_stamp;
    char path[600]; int w = 0, h = 0, n = 0; unsigned char* d = NULL; int decoder_kind = 0;
    if (webpage_image_path(url, path, sizeof path)) {
        FILE* pf = fopen(path, "rb"); int is_svg = 0, is_wp = 0, is_av = 0;
        if (pf) { unsigned char m[4096] = {0}; size_t rn = fread(m, 1, sizeof m, pf); fclose(pf); is_wp = is_webp(m, rn); is_av = is_avif(m, rn); is_svg = svg_looks_like((const char*)m, rn); }
        if (is_svg) {                                                       /* вектор: растеризуем в собственный размер (масштабированные копии — векторно, см. webpage_image_scaled) */
            size_t sl = 0; char* src = read_file_cap(path, 8u << 20, &sl); float fw = 0, fh = 0;
            if (src && svg_intrinsic(src, sl, &fw, &fh)) {
                w = (int)(fw + 0.5f); h = (int)(fh + 0.5f); if (w < 1) w = 1; if (h < 1) h = 1; if (w > 4096) { h = h * 4096 / w; w = 4096; } if (h > 4096) { w = w * 4096 / h; h = 4096; } if (w < 1) w = 1; if (h < 1) h = 1;
                uint32_t* px = svg_render(src, sl, w, h, 0xFF000000u);
                if (px) { free(src); e->pix.px = px; e->pix.w = w; e->pix.h = h; e->svg = 1; e->cc = 0xFF000000u; g_bytes += (size_t)w * h * 4; ic_trim(); return &e->pix; }
            }
            free(src); e->fail = 1; return NULL;
        }
#ifdef HAVE_WEBP
        if (is_wp) { d = load_webp(path, &w, &h); decoder_kind = 1; }
        else
#endif
#ifdef HAVE_AVIF
        if (is_av) { d = load_avif(path, &w, &h); decoder_kind = 2; }
        else
#endif
        if (!is_wp && !is_av) d = stbi_load(path, &w, &h, &n, 4);
    }
    if (!d || w < 1 || h < 1 || (uint64_t)w * (uint64_t)h > 40000000ull) {
        if (d) { if (decoder_kind) free_decoder_buffer(d, decoder_kind); else stbi_image_free(d); }
        e->fail = 1; return NULL;
    }
    uint32_t* px = malloc((size_t)w * h * 4);
    if (!px) { if (decoder_kind) free_decoder_buffer(d, decoder_kind); else stbi_image_free(d); e->fail = 1; return NULL; }
    for (long i = 0; i < (long)w * h; i++) px[i] = ((uint32_t)d[i * 4 + 3] << 24) | ((uint32_t)d[i * 4] << 16) | ((uint32_t)d[i * 4 + 1] << 8) | d[i * 4 + 2];
    if (decoder_kind) free_decoder_buffer(d, decoder_kind); else stbi_image_free(d);
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
    char kb[600]; const char* key = ic_key(url, kb, sizeof kb);
    for (int i = 0; i < IC_MAX; i++) if (g_ic[i].w2 == w && g_ic[i].h2 == h && !strcmp(g_ic[i].url, key) && (!g_ic[i].svg || g_ic[i].cc == g_cc)) { g_ic[i].stamp = ++g_stamp; return &g_ic[i].pix; }
    ImgPix* src = webpage_image_get(url); if (!src) return NULL;
    int src_svg = 0; for (int i = 0; i < IC_MAX; i++) if (&g_ic[i].pix == src) { src_svg = g_ic[i].svg; break; }
    if (src_svg) {                                                          /* вектор: перерисовываем сразу в нужный размер и нужным цветом currentColor */
        if (src->w == w && src->h == h && g_cc == 0xFF000000u) return src;
        char path[600]; size_t sl = 0; char* text = webpage_image_path(url, path, sizeof path) ? read_file_cap(path, 8u << 20, &sl) : NULL;
        uint32_t* px = text ? svg_render(text, sl, w, h, g_cc) : NULL; free(text);
        if (!px) return src;
        ICEntry* e = ic_slot(); memset(e, 0, sizeof *e);
        snprintf(e->url, sizeof e->url, "%s", key); e->w2 = w; e->h2 = h; e->stamp = ++g_stamp; e->svg = 1; e->cc = g_cc;
        e->pix.px = px; e->pix.w = w; e->pix.h = h; g_bytes += (size_t)w * h * 4; ic_trim();
        return &e->pix;
    }
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
    snprintf(e->url, sizeof e->url, "%s", key); e->w2 = w; e->h2 = h; e->stamp = ++g_stamp;
    e->pix.px = out; e->pix.w = w; e->pix.h = h; g_bytes += (size_t)w * h * 4; ic_trim();
    return &e->pix;
}

void webpage_image_cache_clear(void) { for (int i = 0; i < IC_MAX; i++) ic_drop(&g_ic[i]); }
