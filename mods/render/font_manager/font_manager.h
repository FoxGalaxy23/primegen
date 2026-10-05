#ifndef FONT_MANAGER_H
#define FONT_MANAGER_H

#include "ttf_render.h"

void font_manager_init(void);
void font_manager_cleanup(void);
void font_manager_set_offline(int off);   /* 1 — не скачивать шрифты (пересчёт стилей из JS) */

/* Зарегистрировать шрифт (вес 400, normal). Для совместимости. */
void font_manager_register(const char* family, const char* path);

/* Зарегистрировать начертание семейства: weight 100..900, italic 0/1. */
void font_manager_register_face(const char* family, const char* path, int weight, int italic);

/* Найти шрифт по значению CSS font-family — можно передавать целый список:
     "'Open Sans', Arial, sans-serif"
   Берётся первое семейство, которое зарегистрировано и загружается; generic-имена
   (serif, sans-serif, monospace, system-ui ...) дают шрифт по умолчанию.
   Никогда не возвращает шрифт без fallback'а на дефолтный. */
TTFFont* font_manager_get(const char* family);

/* То же, но с выбором начертания (ближайший вес / стиль среди зарегистрированных). */
TTFFont* font_manager_get_styled(const char* family, int weight, int italic);

/* Скачать и зарегистрировать шрифт по одному URL (совместимость со старым API). */
void font_manager_download(const char* family, const char* url, const char* page_url);

/* Обработать @font-face: src — полное значение дескриптора, например
     url(a.woff2) format("woff2"), url(a.woff) format("woff"), url(a.ttf)
   Выбирается лучший из поддерживаемых форматов (TTF/OTF-glyf/TTC/WOFF; WOFF2 не
   поддерживается). weight/style — значения дескрипторов font-weight / font-style
   (могут быть NULL). URL разрешается относительно page_url. */
void font_manager_add_face(const char* family, const char* src,
                           const char* weight, const char* style, const char* page_url);

#endif
