#ifndef DB_H
#define DB_H

#include "parser.h"
#include "ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef DISABLED_MOD_DB

// Global active profile ID variable
extern int active_profile_id;

// DOM helper
DOMNode* find_dom_node_by_id_in_tree(DOMNode* head, const char* id);

// Database initialization
int db_init(void);
void db_close(void);

// Profile management
int db_get_active_profile(char* name_out, int max_len);
int db_set_active_profile(int profile_id);
int db_create_profile(const char* name);
int db_delete_profile(int profile_id);
char* db_generate_profile_html(void);

// History management
int db_add_history_entry(const char* url, const char* title);
int db_delete_history_entry(int id);
int db_clear_history(void);
char* db_generate_history_html(void);

// Cookie management
int db_save_cookie(int profile_id, const char* domain, const char* name, const char* value, const char* path, long long expiry, int is_secure);
int db_delete_cookie(int id);
int db_clear_cookies(void);
void db_export_cookies_to_file(int profile_id, const char* filepath);
void db_import_cookies_from_file(int profile_id, const char* filepath);
char* db_generate_cookies_html(void);

// Personal Info management
int db_save_personal_info(const char* key, const char* value);
char* db_get_personal_info(const char* key);

// localStorage (переживает навигацию/перезапуск): хранится по (профиль, origin, ключ)
int db_ls_set(const char* origin, const char* key, const char* value);
int db_ls_remove(const char* origin, const char* key);
int db_ls_clear(const char* origin);
char* db_ls_list_json(const char* origin);   // {"key":"value",...} для гидратации Storage; вызывающий обязан free()

int db_add_download(const char* url, const char* path, const char* filename, long long total_size);
void db_update_download_progress(int id, long long downloaded, const char* status);
char* db_generate_downloads_html(void);

#else

// Stubs for disabled DB module
static const int active_profile_id = 0;
static inline DOMNode* find_dom_node_by_id_in_tree(DOMNode* head, const char* id) { (void)head; (void)id; return NULL; }
static inline int db_init(void) { return 1; }
static inline void db_close(void) {}
static inline int db_get_active_profile(char* n, int l) { if(n && l>0) n[0]='\0'; return 0; }
static inline int db_set_active_profile(int p) { (void)p; return 0; }
static inline int db_create_profile(const char* n) { (void)n; return 0; }
static inline int db_delete_profile(int p) { (void)p; return 0; }
static inline char* db_generate_profile_html(void) { return strdup("<p>DB module disabled.</p>"); }
static inline int db_add_history_entry(const char* u, const char* t) { (void)u; (void)t; return 0; }
static inline int db_delete_history_entry(int i) { (void)i; return 0; }
static inline int db_clear_history(void) { return 0; }
static inline char* db_generate_history_html(void) { return strdup("<p>History disabled.</p>"); }
static inline int db_save_cookie(int p, const char* d, const char* n, const char* v, const char* pa, long long e, int s) { (void)p;(void)d;(void)n;(void)v;(void)pa;(void)e;(void)s; return 0; }
static inline int db_delete_cookie(int i) { (void)i; return 0; }
static inline int db_clear_cookies(void) { return 0; }
static inline void db_export_cookies_to_file(int p, const char* f) { (void)p;(void)f; }
static inline void db_import_cookies_from_file(int p, const char* f) { (void)p;(void)f; }
static inline char* db_generate_cookies_html(void) { return strdup("<p>Cookies disabled.</p>"); }
static inline int db_save_personal_info(const char* k, const char* v) { (void)k;(void)v; return 0; }
static inline char* db_get_personal_info(const char* k) { (void)k; return NULL; }
static inline int db_ls_set(const char* o, const char* k, const char* v) { (void)o;(void)k;(void)v; return 0; }
static inline int db_ls_remove(const char* o, const char* k) { (void)o;(void)k; return 0; }
static inline int db_ls_clear(const char* o) { (void)o; return 0; }
static inline char* db_ls_list_json(const char* o) { (void)o; return strdup("{}"); }
static inline int db_add_download(const char* u, const char* p, const char* f, long long t) { (void)u;(void)p;(void)f;(void)t; return -1; }
static inline void db_update_download_progress(int i, long long d, const char* s) { (void)i;(void)d;(void)s; }
static inline char* db_generate_downloads_html(void) { return strdup("<p>Downloads disabled.</p>"); }

#endif

#endif
