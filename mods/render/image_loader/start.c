#include "image_loader.h"
#include "gal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <png.h>

// ========== BMP LOADER ==========

#pragma pack(push, 1)
typedef struct {
    uint16_t bfType;
    uint32_t bfSize;
    uint16_t bfReserved1;
    uint16_t bfReserved2;
    uint32_t bfOffBits;
} BMPFileHeader;

typedef struct {
    uint32_t biSize;
    int32_t  biWidth;
    int32_t  biHeight;
    uint16_t biPlanes;
    uint16_t biBitCount;
    uint32_t biCompression;
    uint32_t biSizeImage;
    int32_t  biXPelsPerMeter;
    int32_t  biYPelsPerMeter;
    uint32_t biClrUsed;
    uint32_t biClrImportant;
} BMPInfoHeader;
#pragma pack(pop)

LoadedImage* image_load_bmp(const char* path) {
    FILE* file = fopen(path, "rb");
    if (!file) { gal_log("WARN", "BMP: cannot open %s", path); return NULL; }

    BMPFileHeader fh;
    BMPInfoHeader ih;
    fread(&fh, sizeof(BMPFileHeader), 1, file);
    fread(&ih, sizeof(BMPInfoHeader), 1, file);

    if (fh.bfType != 0x4D42 || ih.biBitCount != 24) {
        gal_log("WARN", "BMP: unsupported format (only 24-bit)");
        fclose(file); return NULL;
    }

    int w = ih.biWidth;
    int h = ih.biHeight;
    int rowPad = (4 - (w * 3) % 4) % 4;

    LoadedImage* img = malloc(sizeof(LoadedImage));
    img->width = w;
    img->height = h;
    img->pixels = malloc(w * h * sizeof(uint32_t));

    uint8_t* rowBuf = malloc(w * 3 + rowPad);
    fseek(file, fh.bfOffBits, SEEK_SET);

    for (int y = 0; y < h; y++) {
        fread(rowBuf, 1, w * 3 + rowPad, file);
        int tY = h - 1 - y;
        for (int x = 0; x < w; x++) {
            uint8_t b = rowBuf[x*3], g = rowBuf[x*3+1], r = rowBuf[x*3+2];
            img->pixels[tY * w + x] = (0xFF << 24) | (r << 16) | (g << 8) | b;
        }
    }

    free(rowBuf);
    fclose(file);
    gal_log("INFO", "BMP loaded: %s (%dx%d)", path, w, h);
    return img;
}

// ========== PNG LOADER (libpng) ==========

LoadedImage* image_load_png(const char* path) {
    FILE* fp = fopen(path, "rb");
    if (!fp) return NULL;

    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) { fclose(fp); return NULL; }

    png_infop info = png_create_info_struct(png);
    if (!info) { png_destroy_read_struct(&png, NULL, NULL); fclose(fp); return NULL; }

    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, NULL);
        fclose(fp);
        return NULL;
    }

    png_init_io(png, fp);
    png_read_info(png, info);

    int width = png_get_image_width(png, info);
    int height = png_get_image_height(png, info);
    png_byte color_type = png_get_color_type(png, info);
    png_byte bit_depth = png_get_bit_depth(png, info);

    if (bit_depth == 16) png_set_strip_16(png);
    if (color_type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);

    if (color_type == PNG_COLOR_TYPE_RGB || color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_filler(png, 0xFF, PNG_FILLER_AFTER);

    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png);

    png_read_update_info(png, info);

    LoadedImage* img = malloc(sizeof(LoadedImage));
    img->width = width;
    img->height = height;
    img->pixels = malloc(width * height * 4);

    png_bytep* row_pointers = (png_bytep*)malloc(sizeof(png_bytep) * height);
    for (int y = 0; y < height; y++) {
        row_pointers[y] = (png_bytep)&img->pixels[y * width];
    }

    png_read_image(png, row_pointers);

    // Convert RGBA (libpng) to ARGB (GAL uses 0xAARRGGBB, but libpng filler after makes it 0xRRGGBBAA or similar)
    // Actually, GAL uses (r<<16)|(g<<8)|b and alpha is top 8 bits.
    // Libpng with PNG_FILLER_AFTER and 8bit depth gives R, G, B, A in memory order.
    // On little-endian, a uint32_t of 0xAABBGGRR would be R, G, B, A in bytes.
    // GAL expects 0xAARRGGBB.

    for (int i = 0; i < width * height; i++) {
        uint8_t* p = (uint8_t*)&img->pixels[i];
        uint8_t r = p[0], g = p[1], b = p[2], a = p[3];
        img->pixels[i] = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
    }

    free(row_pointers);
    png_destroy_read_struct(&png, &info, NULL);
    fclose(fp);

    gal_log("INFO", "PNG loaded (libpng): %s (%dx%d)", path, width, height);
    return img;
}

// ========== AUTO FORMAT DETECTION ==========

static struct {
    char path[512];
    LoadedImage* img;
} g_image_cache[128];
static int g_image_cache_count = 0;

LoadedImage* image_load(const char* path) {
    if (!path) return NULL;

    // Check cache
    for (int i = 0; i < g_image_cache_count; i++) {
        if (strcmp(g_image_cache[i].path, path) == 0) return g_image_cache[i].img;
    }

    size_t len = strlen(path);
    LoadedImage* img = NULL;
    if (len >= 4 && strcasecmp(path + len - 4, ".bmp") == 0) img = image_load_bmp(path);
    else if (len >= 4 && strcasecmp(path + len - 4, ".png") == 0) img = image_load_png(path);
    else {
        img = image_load_bmp(path);
        if (!img) img = image_load_png(path);
    }

    if (img && g_image_cache_count < 128) {
        strncpy(g_image_cache[g_image_cache_count].path, path, 511);
        g_image_cache[g_image_cache_count].img = img;
        g_image_cache_count++;
    }
    return img;
}

void image_free(LoadedImage* img) {
    if (!img) return;
    // Don't free if it's in the cache
    for (int i = 0; i < g_image_cache_count; i++) {
        if (g_image_cache[i].img == img) return;
    }

    if (img->pixels) free(img->pixels);
    free(img);
}

void image_cache_clear(void) {
    for (int i = 0; i < g_image_cache_count; i++) {
        if (g_image_cache[i].img->pixels) free(g_image_cache[i].img->pixels);
        free(g_image_cache[i].img);
    }
    g_image_cache_count = 0;
}
