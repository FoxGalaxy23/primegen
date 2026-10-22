#ifndef NCORS_H
#define NCORS_H
#include <stddef.h>
#include "nhttp.h"

/* Запросы из скриптов страницы (fetch/XMLHttpRequest) с политикой одного источника и CORS (Fetch Standard):
   Origin/Referer/Sec-Fetch-*, запрет «запрещённых» заголовков, куки только по правилам credentials, preflight (OPTIONS) для
   не «простых» запросов, проверка Access-Control-Allow-*, фильтрация заголовков ответа (Set-Cookie не виден никогда,
   межсайтовые — только безопасные и перечисленные в Access-Control-Expose-Headers). Нарушение политики выглядит для скрипта
   как сетевая ошибка (так в браузерах) — подробности в err. */
enum { NCORS_CRED_OMIT = 0, NCORS_CRED_SAME_ORIGIN = 1, NCORS_CRED_INCLUDE = 2 };
enum { NCORS_MODE_CORS = 0, NCORS_MODE_NO_CORS = 1, NCORS_MODE_SAME_ORIGIN = 2 };
typedef struct {
    const char* method; const char* url; const char* headers;               /* заголовки запроса от скрипта: «Имя: значение» через '\n' */
    const unsigned char* body; size_t body_len;
    const char* page_url;                                                    /* страница-инициатор; NULL — доверенный запрос браузера, политика не применяется */
    int credentials, mode;
    NJar* jar;                                                               /* куки пользователя; используется только если политика разрешает отправку */
    int jar_used;                                                            /* (вывод) куки реально участвовали — вызывающий сохранит изменения */
    int timeout_ms;
} NCorsReq;

int ncors_fetch(NCorsReq* r, NHttpResp* out);                                /* 1 — ответ получен и разрешён политикой; 0 — ошибка (out->err) */
void ncors_origin(const char* url, char* out, size_t cap);                   /* "https://host[:порт]" или "null" */
#endif
