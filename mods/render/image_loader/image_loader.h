#ifndef IMAGE_LOADER_H
#define IMAGE_LOADER_H

#include <stdint.h>

// Загруженное изображение (пиксели в формате 0xAARRGGBB)
typedef struct {
    uint32_t* pixels;
    int width;
    int height;
} LoadedImage;

// Загрузить BMP файл
LoadedImage* image_load_bmp(const char* path);

// Загрузить PNG файл (требует zlib)
LoadedImage* image_load_png(const char* path);

// Автоопределение формата по расширению (.bmp / .png)
LoadedImage* image_load(const char* path);

// Освободить изображение
void image_free(LoadedImage* img);

// Очистить кэш изображений
void image_cache_clear(void);

#endif
