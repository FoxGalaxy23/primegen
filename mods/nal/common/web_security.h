#ifndef MLB_WEB_SECURITY_H
#define MLB_WEB_SECURITY_H
#include <stddef.h>
enum {
    NAL_RESOURCE_OTHER = 0,
    NAL_RESOURCE_DOCUMENT,
    NAL_RESOURCE_SCRIPT,
    NAL_RESOURCE_STYLE,
    NAL_RESOURCE_IMAGE,
    NAL_RESOURCE_FONT,
    NAL_RESOURCE_CONNECT,
    NAL_RESOURCE_WORKER
};
void websec_set_document_url(const char *url);
void websec_set_response(const char *document_url, const char *headers);
int websec_is_document_url(const char *url);
int websec_allows(const char *url, int resource_kind);
int websec_allows_inline(int resource_kind, const char *nonce);
/* Returns 1 after upgrading an http:// URL covered by stored HSTS; 0 otherwise. */
int websec_hsts_upgrade_url(const char *url, char *out, size_t cap);
void websec_hsts_remember(const char *host_ascii, int is_https, const char *headers);
#endif
