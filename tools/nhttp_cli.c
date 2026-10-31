/* Мини-утилита для проверки собственного HTTP/TLS-клиента без остального браузера.
   Сборка: gcc -O2 -Imods/nal/common tools/nhttp_cli.c mods/nal/common/nhttp.c mods/nal/common/psl.c mods/nal/common/ntls_openssl.c mods/nal/common/ninflate.c -ldl -o /tmp/nhttp_cli
   Запуск: /tmp/nhttp_cli [-X METHOD] [-d DATA] [-H "K: V"]... [-c JARFILE] [-o FILE] [-i] [-n N] URL
   -n N — повторить запрос N раз (проверка keep-alive пула), -i — печатать заголовки, -c — файл куки (Netscape) читается и обновляется. */
#include "nhttp.h"
#include "ntls.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char** argv) {
    const char *method = NULL, *data = NULL, *jarfile = NULL, *outf = NULL, *url = NULL, *site_for_cookies = NULL; char hdrs[4096] = ""; int show = 0, reps = 1, timeout = 0, policy = 0, clear = 0, top_level = 1; const char* cachedir = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-X") && i + 1 < argc) method = argv[++i];
        else if (!strcmp(argv[i], "-d") && i + 1 < argc) data = argv[++i];
        else if (!strcmp(argv[i], "-H") && i + 1 < argc) { strncat(hdrs, argv[++i], sizeof hdrs - strlen(hdrs) - 2); strcat(hdrs, "\n"); }
        else if (!strcmp(argv[i], "-c") && i + 1 < argc) jarfile = argv[++i];
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) outf = argv[++i];
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) reps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) timeout = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--cache") && i + 1 < argc) cachedir = argv[++i];
        else if (!strcmp(argv[i], "--site-for-cookies") && i + 1 < argc) site_for_cookies = argv[++i];
        else if (!strcmp(argv[i], "--subresource")) top_level = 0;
        else if (!strcmp(argv[i], "--reload")) policy = NHTTP_CACHE_RELOAD;
        else if (!strcmp(argv[i], "--offline")) policy = NHTTP_CACHE_OFFLINE;
        else if (!strcmp(argv[i], "--clear-cache")) clear = 1;
        else if (!strcmp(argv[i], "-i")) show = 1;
        else url = argv[i];
    }
    if (!url) { fprintf(stderr, "usage: nhttp_cli [opts] URL\n"); return 2; }
    if (cachedir) { nhttp_cache_config(cachedir, 0); nhttp_cache_policy(policy); if (clear) nhttp_cache_clear(); }
    NJar* jar = njar_new(); if (jarfile) njar_load_netscape(jar, jarfile);
    int rc = 0;
    for (int n = 0; n < reps; n++) {
        FILE* sink = outf ? fopen(outf, "wb") : NULL;
        NHttpReq rq = {0}; rq.method = method; rq.url = url; rq.headers = hdrs[0] ? hdrs : NULL; rq.body = (const unsigned char*)data; rq.body_len = data ? strlen(data) : 0; rq.jar = jar; rq.sink = sink; rq.timeout_ms = timeout; /* CLI requests model a top-level navigation; browser callers supply their real initiator site. */ rq.site_for_cookies = site_for_cookies ? site_for_cookies : url; rq.is_top_level_navigation = top_level;
        NHttpResp rs;
        int ok = nhttp_perform(&rq, &rs);
        if (sink) fclose(sink);
        if (!ok) { fprintf(stderr, "ERROR: %s\n", rs.err); rc = 1; nhttp_resp_free(&rs); continue; }
        fprintf(stderr, "[%d] status=%d body=%zu final=%s tls=%s cache=%s\n", n, rs.status, outf ? rs.sink_bytes : rs.body_len, rs.final_url, ntls_backend_name(), rs.from_cache == 1 ? "hit" : rs.from_cache == 2 ? "revalidated" : "miss");
        if (show && n == reps - 1) { fprintf(stderr, "--- headers (last) ---\n%s", rs.headers); if (rs.all_headers) fprintf(stderr, "--- all blocks ---\n%s", rs.all_headers); }
        if (!outf && rs.body && n == reps - 1) fwrite(rs.body, 1, rs.body_len, stdout);
        nhttp_resp_free(&rs);
    }
    if (jarfile) njar_save_all(jar, jarfile);
    njar_free(jar); return rc;
}
