/* Проверка политики CORS без браузера.
   Сборка: gcc -O2 -Imods/nal/common tools/ncors_cli.c mods/nal/common/{ncors,nhttp,psl,ntls_openssl,ninflate}.c -ldl -o /tmp/ncors_cli
   Запуск: ncors_cli --page URL [--cred 0|1|2] [--mode 0|1|2] [-X M] [-H "K: V"]... [-d DATA] [-c jar] [-i] URL */
#include "ncors.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char** argv) {
    NCorsReq r; memset(&r, 0, sizeof r); r.credentials = 1; char hdrs[2048] = ""; const char *jarf = NULL, *data = NULL; int show = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--page") && i + 1 < argc) r.page_url = argv[++i]; else if (!strcmp(argv[i], "--cred") && i + 1 < argc) r.credentials = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--mode") && i + 1 < argc) r.mode = atoi(argv[++i]); else if (!strcmp(argv[i], "-X") && i + 1 < argc) r.method = argv[++i];
        else if (!strcmp(argv[i], "-H") && i + 1 < argc) { strncat(hdrs, argv[++i], sizeof hdrs - strlen(hdrs) - 2); strcat(hdrs, "\n"); }
        else if (!strcmp(argv[i], "-d") && i + 1 < argc) data = argv[++i]; else if (!strcmp(argv[i], "-c") && i + 1 < argc) jarf = argv[++i]; else if (!strcmp(argv[i], "-i")) show = 1; else r.url = argv[i];
    }
    r.headers = hdrs[0] ? hdrs : NULL; r.body = (const unsigned char*)data; r.body_len = data ? strlen(data) : 0;
    NJar* jar = njar_new(); if (jarf) njar_load_netscape(jar, jarf); r.jar = jar;
    NHttpResp rs; int ok = ncors_fetch(&r, &rs);
    if (!ok) { printf("ERROR: %s\n", rs.err); return 1; }
    printf("status=%d body=%.*s\n", rs.status, (int)rs.body_len, rs.body ? (const char*)rs.body : ""); if (show) printf("--headers--\n%s", rs.headers);
    printf("jar_used=%d\n", r.jar_used); return 0;
}
