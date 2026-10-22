#ifndef NHTTP_H
#define NHTTP_H
#include <stddef.h>
#include <stdio.h>

/* Собственный HTTP/1.1-клиент: DNS, TCP (таймауты), TLS через ntls.h, keep-alive пул соединений, chunked,
   gzip/deflate, редиректы, куки по RFC 6265 (в памяти). Блокирующий; для асинхронности NAL вызывает его в fork-потомке. */

/* ── cookie jar в памяти ── */
typedef struct NJar NJar;
NJar* njar_new(void);
void  njar_free(NJar* j);
/* Загрузка/выгрузка в формате Netscape (тот же, что db_export/import_cookies_*): домен с ведущей точкой — для поддоменов,
   без точки — только этот хост. Просроченные при загрузке отбрасываются. */
void  njar_load_netscape(NJar* j, const char* path);
int   njar_save_all(NJar* j, const char* path);        /* все непросроченные куки (для jar в файле без БД) */
int   njar_save_changed(NJar* j, const char* path);    /* только новые/изменённые за время жизни jar; удалённые пишутся с expiry=1; возвращает число строк */

typedef struct {
    const char* method;               /* NULL → "GET" */
    const char* url;
    const char* headers;              /* "Ключ: значение" через '\n' (может быть NULL); совпадающие по имени заменяют стандартные */
    const unsigned char* body; size_t body_len;
    NJar* jar;                        /* может быть NULL — тогда куки не отправляются и не сохраняются */
    int max_redirects;                /* 0 → 20; <0 — не следовать */
    int timeout_ms;                   /* общий лимит на всю цепочку; 0 → 90000 */
    FILE* sink;                       /* не NULL — тело пишется в файл по мере чтения (без распаковки, Accept-Encoding: identity) */
    size_t max_body;                  /* 0 → 256 МиБ */
    const char* user_agent;           /* NULL → стандартный Chrome-подобный */
} NHttpReq;

typedef struct {
    int status;                       /* HTTP-код последнего ответа; 0 — ответа нет (см. err) */
    char* headers;                    /* заголовки последнего ответа (без Content-Encoding/Transfer-Encoding/Content-Length — тело уже декодировано) */
    char* all_headers;                /* блоки заголовков всех ответов цепочки подряд (как curl -D при -L) */
    unsigned char* body; size_t body_len;   /* malloc; всегда с завершающим '\0' (не входит в body_len); NULL если sink */
    size_t sink_bytes;                /* сколько байт записано в sink */
    char final_url[2048];
    int from_cache;                   /* последнее звено цепочки: 0 — сеть, 1 — свежая запись кэша без запроса, 2 — кэш после проверки (304) */
    char err[200];
} NHttpResp;

int  nhttp_perform(const NHttpReq* req, NHttpResp* out);    /* 1 — получен HTTP-ответ любого статуса; 0 — сетевая/TLS-ошибка */
void nhttp_resp_free(NHttpResp* r);
void nhttp_pool_clear(void);

/* ── HTTP-кэш (приватный, на диске, RFC 9111): GET без тела и без пользовательских заголовков ──
   Свежие записи отдаются без запроса, устаревшие проверяются условным запросом (ETag/Last-Modified → 304). Уважаются Cache-Control
   (no-store, no-cache, max-age, immutable), Expires, Vary (кэшируются только ответы без Vary или с Vary по Accept и User-Agent),
   эвристика по Last-Modified (10 %, не более суток; без неё для URL с «?» ответ не считается свежим). */
enum { NHTTP_CACHE_NORMAL = 0, NHTTP_CACHE_RELOAD = 1, NHTTP_CACHE_OFFLINE = 2 };   /* RELOAD — всё проверять заново; OFFLINE — отдавать из кэша даже устаревшее */
void   nhttp_cache_config(const char* dir, size_t max_bytes);    /* dir == NULL — кэш выключен; max_bytes — предел размера на диске */
void   nhttp_cache_policy(int policy);
void   nhttp_cache_clear(void);
void   nhttp_cache_stats(size_t* entries, size_t* bytes);
#endif
