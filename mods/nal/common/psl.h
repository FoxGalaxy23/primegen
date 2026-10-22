#ifndef PSL_H
#define PSL_H
#include <stddef.h>
/* Public Suffix List: какие домены общие (com, co.uk, github.io …). Нужен, чтобы сайт не мог поставить куки на «чужой» домен
   верхнего уровня и чтобы корректно определять «тот же сайт» (eTLD+1). Данные встроены (psl_data.inc, tools/gen_psl.py). */
int psl_is_public_suffix(const char* domain);                                   /* домен целиком — публичный суффикс */
int psl_registrable_domain(const char* host, char* out, size_t cap);             /* eTLD+1; 0 — у хоста нет регистрируемого домена (сам суффикс, IP, localhost) */
#endif
