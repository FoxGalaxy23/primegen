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
/* Consistent SQLite profile backup/restore. Imported backups are schema-validated before replacing the live database. */
int db_export_profile_backup(const char* path);
int db_import_profile_backup(const char* path);

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

/* Browser UI persistence */
int db_bookmark_add(const char* url, const char* title);
int db_bookmark_delete(int id);
int db_bookmark_delete_url(const char* url);
int db_bookmark_exists(const char* url);
char* db_generate_bookmarks_html(void);
int db_save_session_tab(int slot, const char* url, int is_active);
int db_session_count(void);
int db_session_get_url(int slot, char* out, size_t cap, int* is_active);
int db_session_clear_from(int first_slot);

/* Password vault: ciphertext only in SQLite; requires a local protected vault key. */
int db_password_save(const char* origin, const char* username, const char* password);
int db_password_get_for_origin(const char* origin, char* username, size_t ucap, char* password, size_t pcap);
int db_password_delete(int id);
char* db_generate_passwords_html(void);

// Cookie management
int db_save_cookie(int profile_id, const char* domain, const char* name, const char* value, const char* path, long long expiry, int is_secure, int same_site);
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

// IndexedDB: метаданные и записи хранятся отдельно от localStorage, с изоляцией по профилю и origin.
char* db_idb_meta_list(const char* origin);
char* db_idb_meta_get(const char* origin, const char* name);
int db_idb_meta_set(const char* origin, const char* name, int version, const char* stores_json);
int db_idb_delete_database(const char* origin, const char* name);
char* db_idb_list(const char* origin, const char* name, const char* store);
char* db_idb_get(const char* origin, const char* name, const char* store, const char* key);
int db_idb_put(const char* origin, const char* name, const char* store, const char* key, const char* value_json);
int db_idb_remove(const char* origin, const char* name, const char* store, const char* key);
int db_idb_clear(const char* origin, const char* name, const char* store);

// Реестр Service Worker: регистрация переживает загрузки страниц и перезапуск.
char* db_sw_list(const char* origin);
int db_sw_set(const char* origin, const char* scope, const char* script_url);
int db_sw_remove(const char* origin, const char* scope);

typedef struct {
    int id;
    char url[8192];
    char path[4096];
    char etag[1024];
    char last_modified[256];
    long long downloaded_size;
    long long total_size;
    int accept_ranges;
} DBDownloadResumeInfo;
int db_add_download(const char* url, const char* path, const char* filename, long long total_size);
void db_update_download_progress(int id, long long downloaded, const char* status);
int db_download_set_validators(int id, const char* etag, const char* last_modified, int accept_ranges);
int db_download_update_response(int id, long long total_size, const char* etag, const char* last_modified, int accept_ranges);
void db_download_recover_stale(void);
/* Resume metadata is returned only for interrupted rows with a stable validator and consistent file size. */
int db_download_get_resume_info(int id, DBDownloadResumeInfo* out);
char* db_generate_downloads_html(void);

#else

// Stubs for disabled DB module
static const int active_profile_id = 0;
static inline DOMNode* find_dom_node_by_id_in_tree(DOMNode* head, const char* id) { (void)head; (void)id; return NULL; }
static inline int db_init(void) { return 1; }
static inline void db_close(void) {}
static inline int db_export_profile_backup(const char* p) { (void)p; return 0; }
static inline int db_import_profile_backup(const char* p) { (void)p; return 0; }
static inline int db_get_active_profile(char* n, int l) { if(n && l>0) n[0]='\0'; return 0; }
static inline int db_set_active_profile(int p) { (void)p; return 0; }
static inline int db_create_profile(const char* n) { (void)n; return 0; }
static inline int db_delete_profile(int p) { (void)p; return 0; }
static inline char* db_generate_profile_html(void) { return strdup("<p>DB module disabled.</p>"); }
static inline int db_add_history_entry(const char* u, const char* t) { (void)u; (void)t; return 0; }
static inline int db_delete_history_entry(int i) { (void)i; return 0; }
static inline int db_clear_history(void) { return 0; }
static inline char* db_generate_history_html(void) { return strdup("<p>History disabled.</p>"); }
static inline int db_bookmark_add(const char* u,const char* t) { (void)u;(void)t;return 0; }
static inline int db_bookmark_delete(int i) { (void)i;return 0; }
static inline int db_bookmark_delete_url(const char* u) { (void)u;return 0; }
static inline int db_bookmark_exists(const char* u) { (void)u;return 0; }
static inline char* db_generate_bookmarks_html(void) { return strdup("<p>Bookmarks disabled.</p>"); }
static inline int db_save_session_tab(int s,const char* u,int a) { (void)s;(void)u;(void)a;return 0; }
static inline int db_session_count(void) { return 0; }
static inline int db_session_get_url(int s,char* o,size_t c,int* a) { (void)s;if(o&&c)o[0]=0;if(a)*a=0;return 0; }
static inline int db_session_clear_from(int s) { (void)s;return 0; }
static inline int db_password_save(const char* o,const char* u,const char* p) { (void)o;(void)u;(void)p;return 0; }
static inline int db_password_get_for_origin(const char* o,char* u,size_t uc,char* p,size_t pc) { (void)o;if(u&&uc)u[0]=0;if(p&&pc)p[0]=0;return 0; }
static inline int db_password_delete(int i) { (void)i;return 0; }
static inline char* db_generate_passwords_html(void) { return strdup("<p>Password vault disabled.</p>"); }
static inline int db_save_cookie(int p, const char* d, const char* n, const char* v, const char* pa, long long e, int s, int ss) { (void)p;(void)d;(void)n;(void)v;(void)pa;(void)e;(void)s;(void)ss; return 0; }
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
static inline char* db_idb_meta_list(const char* o) { (void)o; return strdup("[]"); }
static inline char* db_idb_meta_get(const char* o, const char* n) { (void)o;(void)n; return NULL; }
static inline int db_idb_meta_set(const char* o,const char* n,int v,const char* s) { (void)o;(void)n;(void)v;(void)s; return 0; }
static inline int db_idb_delete_database(const char* o,const char* n) { (void)o;(void)n; return 0; }
static inline char* db_idb_list(const char* o,const char* n,const char* s) { (void)o;(void)n;(void)s; return strdup("[]"); }
static inline char* db_idb_get(const char* o,const char* n,const char* s,const char* k) { (void)o;(void)n;(void)s;(void)k; return NULL; }
static inline int db_idb_put(const char* o,const char* n,const char* s,const char* k,const char* v) { (void)o;(void)n;(void)s;(void)k;(void)v; return 0; }
static inline int db_idb_remove(const char* o,const char* n,const char* s,const char* k) { (void)o;(void)n;(void)s;(void)k; return 0; }
static inline int db_idb_clear(const char* o,const char* n,const char* s) { (void)o;(void)n;(void)s; return 0; }
static inline char* db_sw_list(const char* o) { (void)o; return strdup("[]"); }
static inline int db_sw_set(const char* o,const char* s,const char* u) { (void)o;(void)s;(void)u; return 0; }
static inline int db_sw_remove(const char* o,const char* s) { (void)o;(void)s; return 0; }
static inline int db_add_download(const char* u, const char* p, const char* f, long long t) { (void)u;(void)p;(void)f;(void)t; return -1; }
static inline void db_update_download_progress(int i, long long d, const char* s) { (void)i;(void)d;(void)s; }
static inline int db_download_set_validators(int i, const char* e, const char* m, int a) { (void)i;(void)e;(void)m;(void)a; return 0; }
static inline int db_download_update_response(int i, long long t, const char* e, const char* m, int a) { (void)i;(void)t;(void)e;(void)m;(void)a; return 0; }
static inline void db_download_recover_stale(void) {}
static inline int db_download_get_resume_info(int i, DBDownloadResumeInfo* o) { (void)i;(void)o; return 0; }
static inline char* db_generate_downloads_html(void) { return strdup("<p>Downloads disabled.</p>"); }

#endif

#endif
