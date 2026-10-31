#include "db.h"
#if __has_include(<sqlite3.h>)
#include <sqlite3.h>
#define USE_SQLITE
#endif

int active_profile_id = 1;

#ifdef USE_SQLITE
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#ifdef HAVE_OPENSSL_VAULT
#include <openssl/evp.h>
#include <openssl/rand.h>
#endif

static sqlite3* db = NULL;

// Helper to find a DOM node by ID recursively (or flat list in our case)
DOMNode* find_dom_node_by_id_in_tree(DOMNode* head, const char* id) {
    DOMNode* curr = head;
    while (curr) {
        if (strcmp(curr->id, id) == 0) return curr;
        curr = curr->next;
    }
    return NULL;
}

int db_init(void) {
    int rc = sqlite3_open("browser.db", &db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Cannot open database: %s\n", sqlite3_errmsg(db));
        return 0;
    }

    // Enable foreign keys
    sqlite3_exec(db, "PRAGMA foreign_keys = ON;", NULL, NULL, NULL);

    // Create tables
    const char* sql_profiles = 
        "CREATE TABLE IF NOT EXISTS profiles ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  name TEXT UNIQUE,"
        "  is_active INTEGER DEFAULT 0"
        ");";
    
    const char* sql_history = 
        "CREATE TABLE IF NOT EXISTS history ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  profile_id INTEGER,"
        "  url TEXT,"
        "  title TEXT,"
        "  visit_time INTEGER,"
        "  FOREIGN KEY(profile_id) REFERENCES profiles(id) ON DELETE CASCADE"
        ");";

    const char* sql_cookies = 
        "CREATE TABLE IF NOT EXISTS cookies ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  profile_id INTEGER,"
        "  domain TEXT,"
        "  name TEXT,"
        "  value TEXT,"
        "  path TEXT,"
        "  expiry INTEGER,"
        "  is_secure INTEGER,"
        "  FOREIGN KEY(profile_id) REFERENCES profiles(id) ON DELETE CASCADE,"
        "  UNIQUE(profile_id, domain, name, path)"
        ");";

    const char* sql_personal = 
        "CREATE TABLE IF NOT EXISTS personal_info ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  profile_id INTEGER,"
        "  key TEXT,"
        "  value TEXT,"
        "  FOREIGN KEY(profile_id) REFERENCES profiles(id) ON DELETE CASCADE,"
        "  UNIQUE(profile_id, key)"
        ");";

    const char* sql_local_storage =                                  /* localStorage: переживает переход на другую страницу и перезапуск браузера */
        "CREATE TABLE IF NOT EXISTS local_storage ("
        "  profile_id INTEGER,"
        "  origin TEXT,"
        "  key TEXT,"
        "  value TEXT,"
        "  UNIQUE(profile_id, origin, key)"
        ");";

    const char* sql_indexed_db_meta =
        "CREATE TABLE IF NOT EXISTS indexed_db_meta ("
        "  profile_id INTEGER, origin TEXT, name TEXT, version INTEGER NOT NULL, stores_json TEXT NOT NULL,"
        "  UNIQUE(profile_id, origin, name)"
        ");";
    const char* sql_indexed_db_records =
        "CREATE TABLE IF NOT EXISTS indexed_db_records ("
        "  profile_id INTEGER, origin TEXT, db_name TEXT, store_name TEXT, key TEXT, value_json TEXT NOT NULL,"
        "  UNIQUE(profile_id, origin, db_name, store_name, key)"
        ");";
    const char* sql_service_workers =
        "CREATE TABLE IF NOT EXISTS service_workers ("
        "  profile_id INTEGER, origin TEXT, scope TEXT, script_url TEXT, updated_at INTEGER DEFAULT 0,"
        "  UNIQUE(profile_id, origin, scope)"
        ");";
    const char* sql_service_worker_caches =
        "CREATE TABLE IF NOT EXISTS service_worker_caches ("
        "  profile_id INTEGER, origin TEXT, scope TEXT, cache_name TEXT, created_at INTEGER DEFAULT 0,"
        "  UNIQUE(profile_id, origin, scope, cache_name)"
        ");";
    const char* sql_service_worker_cache_entries =
        "CREATE TABLE IF NOT EXISTS service_worker_cache_entries ("
        "  profile_id INTEGER, origin TEXT, scope TEXT, cache_name TEXT, request_key TEXT, response_json TEXT NOT NULL,"
        "  UNIQUE(profile_id, origin, scope, cache_name, request_key)"
        ");";

    const char* sql_downloads =
        "CREATE TABLE IF NOT EXISTS downloads ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  url TEXT,"
        "  path TEXT,"
        "  filename TEXT,"
        "  total_size INTEGER,"
        "  downloaded_size INTEGER,"
        "  status TEXT,"
        "  start_time INTEGER,"
        "  etag TEXT NOT NULL DEFAULT '',"
        "  last_modified TEXT NOT NULL DEFAULT '',"
        "  accept_ranges INTEGER NOT NULL DEFAULT 0"
        ");";

    sqlite3_exec(db, sql_profiles, NULL, NULL, NULL);
    sqlite3_exec(db, sql_history, NULL, NULL, NULL);
    sqlite3_exec(db, sql_cookies, NULL, NULL, NULL);
    /* Migrate databases created before SameSite was persisted. Duplicate-column errors are expected on newer DBs. */
    sqlite3_exec(db, "ALTER TABLE cookies ADD COLUMN same_site INTEGER NOT NULL DEFAULT 2;", NULL, NULL, NULL);
    sqlite3_exec(db, sql_personal, NULL, NULL, NULL);
    sqlite3_exec(db, sql_local_storage, NULL, NULL, NULL);
    sqlite3_exec(db, sql_indexed_db_meta, NULL, NULL, NULL);
    sqlite3_exec(db, sql_indexed_db_records, NULL, NULL, NULL);
    sqlite3_exec(db, sql_service_workers, NULL, NULL, NULL);
    sqlite3_exec(db, sql_service_worker_caches, NULL, NULL, NULL);
    sqlite3_exec(db, sql_service_worker_cache_entries, NULL, NULL, NULL);
    sqlite3_exec(db, sql_downloads, NULL, NULL, NULL);
    /* Additive migrations for databases created by older builds. Duplicate-column errors are expected. */
    sqlite3_exec(db, "ALTER TABLE downloads ADD COLUMN etag TEXT NOT NULL DEFAULT '';", NULL, NULL, NULL);
    sqlite3_exec(db, "ALTER TABLE downloads ADD COLUMN last_modified TEXT NOT NULL DEFAULT '';", NULL, NULL, NULL);
    sqlite3_exec(db, "ALTER TABLE downloads ADD COLUMN accept_ranges INTEGER NOT NULL DEFAULT 0;", NULL, NULL, NULL);
    sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS bookmarks (id INTEGER PRIMARY KEY AUTOINCREMENT, profile_id INTEGER NOT NULL, url TEXT NOT NULL, title TEXT NOT NULL DEFAULT '', created_at INTEGER DEFAULT 0, UNIQUE(profile_id,url), FOREIGN KEY(profile_id) REFERENCES profiles(id) ON DELETE CASCADE);", NULL, NULL, NULL);
    sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS browser_session (profile_id INTEGER NOT NULL, slot INTEGER NOT NULL, url TEXT NOT NULL, is_active INTEGER DEFAULT 0, UNIQUE(profile_id,slot), FOREIGN KEY(profile_id) REFERENCES profiles(id) ON DELETE CASCADE);", NULL, NULL, NULL);
    sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS saved_passwords (id INTEGER PRIMARY KEY AUTOINCREMENT, profile_id INTEGER NOT NULL, origin TEXT NOT NULL, username TEXT NOT NULL, password_enc TEXT NOT NULL, created_at INTEGER DEFAULT 0, UNIQUE(profile_id,origin,username), FOREIGN KEY(profile_id) REFERENCES profiles(id) ON DELETE CASCADE);", NULL, NULL, NULL);

    // Insert Default profile if empty
    sqlite3_exec(db, "INSERT OR IGNORE INTO profiles (id, name, is_active) VALUES (1, 'Default', 1);", NULL, NULL, NULL);

    // Find active profile
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db, "SELECT id FROM profiles WHERE is_active = 1 LIMIT 1;", -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            active_profile_id = sqlite3_column_int(stmt, 0);
        } else {
            active_profile_id = 1;
            sqlite3_exec(db, "UPDATE profiles SET is_active = 1 WHERE id = 1;", NULL, NULL, NULL);
        }
        sqlite3_finalize(stmt);
    }

    return 1;
}

void db_close(void) {
    if (db) {
        sqlite3_close(db);
        db = NULL;
    }
}

static int db_backup_all(sqlite3* destination, sqlite3* source) {
    if (!destination || !source) return 0;
    sqlite3_backup* backup = sqlite3_backup_init(destination, "main", source, "main");
    if (!backup) return 0;
    int rc = SQLITE_OK, attempts = 0;
    do {
        rc = sqlite3_backup_step(backup, 256);
        if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED) { sqlite3_sleep(20); attempts++; }
    } while ((rc == SQLITE_OK || rc == SQLITE_BUSY || rc == SQLITE_LOCKED) && attempts < 250);
    int finish_rc = sqlite3_backup_finish(backup);
    return rc == SQLITE_DONE && finish_rc == SQLITE_OK;
}

static int db_path_is_live(const char* path) {
    if (!db || !path) return 0;
    const char* live = sqlite3_db_filename(db, "main");
    if (!live || !*live) return 0;
    if (!strcmp(live, path)) return 1;
#ifndef _WIN32
    char a[PATH_MAX], b[PATH_MAX];
    if (realpath(live, a) && realpath(path, b) && !strcmp(a, b)) return 1;
#endif
    return 0;
}

int db_export_profile_backup(const char* path) {
    if (!db || !path || !*path || db_path_is_live(path)) return 0;
    char tmp[4096];
    if (snprintf(tmp, sizeof tmp, "%s.tmp-%ld", path, (long)getpid()) >= (int)sizeof tmp) return 0;
    remove(tmp);
    sqlite3* dst = NULL;
    if (sqlite3_open_v2(tmp, &dst, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, NULL) != SQLITE_OK) {
        if (dst) sqlite3_close(dst);
        remove(tmp); return 0;
    }
    int ok = db_backup_all(dst, db);
    if (sqlite3_close(dst) != SQLITE_OK) ok = 0;
    if (ok) {
#ifdef _WIN32
        (void)remove(path);
#endif
        if (rename(tmp, path) != 0) ok = 0;
    }
    if (!ok) remove(tmp);
    return ok;
}

static int db_backup_has_core_schema(sqlite3* source) {
    const char* tables[] = {"profiles", "history", "bookmarks"};
    for (size_t i = 0; i < sizeof tables / sizeof tables[0]; ++i) {
        sqlite3_stmt* stmt = NULL;
        if (sqlite3_prepare_v2(source, "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?1", -1, &stmt, NULL) != SQLITE_OK) return 0;
        sqlite3_bind_text(stmt, 1, tables[i], -1, SQLITE_STATIC);
        int found = sqlite3_step(stmt) == SQLITE_ROW;
        sqlite3_finalize(stmt);
        if (!found) return 0;
    }
    sqlite3_stmt* check = NULL;
    if (sqlite3_prepare_v2(source, "PRAGMA quick_check", -1, &check, NULL) != SQLITE_OK) return 0;
    int rc = sqlite3_step(check);
    const unsigned char* value = rc == SQLITE_ROW ? sqlite3_column_text(check, 0) : NULL;
    int ok = value && !strcmp((const char*)value, "ok");
    sqlite3_finalize(check);
    return ok;
}

int db_import_profile_backup(const char* path) {
    if (!db || !path || !*path || db_path_is_live(path)) return 0;
    sqlite3* source = NULL;
    if (sqlite3_open_v2(path, &source, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, NULL) != SQLITE_OK) {
        if (source) sqlite3_close(source);
        return 0;
    }
    if (!db_backup_has_core_schema(source)) { sqlite3_close(source); return 0; }
    (void)mkdir("cache", 0700);
    char safety[256]; snprintf(safety, sizeof safety, "cache/profile-restore-safety-%ld.db", (long)getpid());
    remove(safety);
    if (!db_export_profile_backup(safety)) { sqlite3_close(source); return 0; }
    int ok = db_backup_all(db, source);
    sqlite3_close(source);
    if (ok) {
        db_close();
        ok = db_init(); /* re-run additive schema migrations and refresh active_profile_id */
    }
    if (!ok) {
        sqlite3* rollback = NULL;
        if (sqlite3_open_v2(safety, &rollback, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, NULL) == SQLITE_OK) {
            if (!db) (void)db_init();
            if (db) (void)db_backup_all(db, rollback);
            sqlite3_close(rollback);
        }
        if (db) { db_close(); (void)db_init(); }
    }
    remove(safety);
    return ok;
}

int db_get_active_profile(char* name_out, int max_len) {
    sqlite3_stmt* stmt;
    int success = 0;
    if (sqlite3_prepare_v2(db, "SELECT name FROM profiles WHERE id = ?;", -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, active_profile_id);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* name = (const char*)sqlite3_column_text(stmt, 0);
            if (name) {
                strncpy(name_out, name, max_len - 1);
                name_out[max_len - 1] = '\0';
                success = 1;
            }
        }
        sqlite3_finalize(stmt);
    }
    return success;
}

int db_set_active_profile(int profile_id) {
    // Verify profile exists
    sqlite3_stmt* stmt;
    int exists = 0;
    if (sqlite3_prepare_v2(db, "SELECT id FROM profiles WHERE id = ?;", -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, profile_id);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            exists = 1;
        }
        sqlite3_finalize(stmt);
    }

    if (!exists) return 0;

    sqlite3_exec(db, "UPDATE profiles SET is_active = 0;", NULL, NULL, NULL);
    
    sqlite3_prepare_v2(db, "UPDATE profiles SET is_active = 1 WHERE id = ?;", -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, profile_id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    active_profile_id = profile_id;
    return 1;
}

int db_create_profile(const char* name) {
    if (!name || strlen(name) == 0) return 0;
    sqlite3_stmt* stmt;
    const char* sql = "INSERT INTO profiles (name, is_active) VALUES (?, 0);";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE);
}

int db_delete_profile(int profile_id) {
    if (profile_id == active_profile_id) return 0; // Cannot delete active profile
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, "DELETE FROM profiles WHERE id = ?;", -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, profile_id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE);
}

char* db_generate_profile_html(void) {
    char active_name[64] = "Unknown";
    db_get_active_profile(active_name, sizeof(active_name));

    char* html = malloc(16384);
    strcpy(html, 
        "<h>Profile and Personal Settings</h>"
        "<p>Manage profiles and user information.</p>"
        "<a href=\"localhost/home\">Back to Start Page</a>"
        "<p>Current Active Profile: <b>");
    strcat(html, active_name);
    strcat(html, "</b></p>");

    // List profiles
    strcat(html, "<p><b>Available Profiles:</b></p>");
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db, "SELECT id, name, is_active FROM profiles;", -1, &stmt, NULL) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            int id = sqlite3_column_int(stmt, 0);
            const char* name = (const char*)sqlite3_column_text(stmt, 1);
            int is_active = sqlite3_column_int(stmt, 2);

            char buf[512];
            if (is_active) {
                snprintf(buf, sizeof(buf), "<p>- %s (Active)</p>", name);
            } else {
                snprintf(buf, sizeof(buf), 
                         "<p>- %s <button onclick=\"Browser.switchProfile(%d)\">Switch</button> "
                         "<button onclick=\"Browser.deleteProfile(%d)\">Delete</button></p>", 
                         name, id, id);
            }
            strcat(html, buf);
        }
        sqlite3_finalize(stmt);
    }

    // Add Profile Form
    strcat(html, 
        "<p><b>Create New Profile:</b></p>"
        "<p>Profile Name: <input id=\"new-profile-name\" value=\"\"> "
        "<button onclick=\"Browser.createProfile('new-profile-name')\">Create Profile</button></p>");

    // Personal Info autofill settings
    strcat(html, "<p><b>Personal Info (Autofill):</b></p>");
    
    const char* keys[] = {"first_name", "last_name", "email", "phone", "address"};
    const char* labels[] = {"First Name", "Last Name", "Email Address", "Phone Number", "Physical Address"};
    for (int i = 0; i < 5; i++) {
        char* val = db_get_personal_info(keys[i]);
        char val_buf[256] = {0};
        if (val) {
            strncpy(val_buf, val, sizeof(val_buf) - 1);
            free(val);
        }
        char buf[512];
        snprintf(buf, sizeof(buf), 
                 "<p>%s: <input id=\"info-%s\" value=\"%s\"> "
                 "<button onclick=\"Browser.savePersonalInfo('%s', 'info-%s')\">Save</button></p>",
                 labels[i], keys[i], val_buf, keys[i], keys[i]);
        strcat(html, buf);
    }

    return html;
}

int db_add_history_entry(const char* url, const char* title) {
    if (!url || strlen(url) == 0) return 0;
    sqlite3_stmt* stmt;
    const char* sql = "INSERT INTO history (profile_id, url, title, visit_time) VALUES (?, ?, ?, ?);";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int(stmt, 1, active_profile_id);
    sqlite3_bind_text(stmt, 2, url, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, title ? title : url, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 4, (long long)time(NULL));
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE);
}

int db_delete_history_entry(int id) {
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, "DELETE FROM history WHERE id = ?;", -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE);
}

int db_clear_history(void) {
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, "DELETE FROM history WHERE profile_id = ?;", -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, active_profile_id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE);
}

char* db_generate_history_html(void) {
    char* html = malloc(16384);
    strcpy(html, 
        "<h>Browser History</h>"
        "<p>List of recently visited websites and their metadata.</p>"
        "<a href=\"localhost/home\">Back to Start Page</a> | "
        "<button onclick=\"Browser.clearHistory()\">Clear All History</button><p></p>");

    sqlite3_stmt* stmt;
    const char* sql = "SELECT id, url, title, visit_time FROM history WHERE profile_id = ? ORDER BY visit_time DESC LIMIT 50;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, active_profile_id);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            int id = sqlite3_column_int(stmt, 0);
            const char* url = (const char*)sqlite3_column_text(stmt, 1);
            const char* title = (const char*)sqlite3_column_text(stmt, 2);
            long long visit_time = sqlite3_column_int64(stmt, 3);

            time_t t = (time_t)visit_time;
            struct tm* tm_info = localtime(&t);
            char time_str[32];
            strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);

            char buf[1024];
            snprintf(buf, sizeof(buf), 
                     "<p>[%s] <b>%s</b> - <a href=\"%s\">%s</a> "
                     "<button onclick=\"Browser.deleteHistory(%d)\">Delete</button></p>",
                     time_str, title ? title : "No Title", url, url, id);
            strcat(html, buf);
        }
        sqlite3_finalize(stmt);
    }
    return html;
}

int db_save_cookie(int profile_id, const char* domain, const char* name, const char* value, const char* path, long long expiry, int is_secure, int same_site) {
    sqlite3_stmt* stmt;
    const char* sql = "INSERT OR REPLACE INTO cookies (profile_id, domain, name, value, path, expiry, is_secure, same_site) VALUES (?, ?, ?, ?, ?, ?, ?, ?);";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    
    sqlite3_bind_int(stmt, 1, profile_id);
    sqlite3_bind_text(stmt, 2, domain, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, value, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, path ? path : "/", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 6, expiry);
    sqlite3_bind_int(stmt, 7, is_secure);
    sqlite3_bind_int(stmt, 8, (same_site >= 1 && same_site <= 3) ? same_site : 2);
    
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE);
}

int db_delete_cookie(int id) {
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, "DELETE FROM cookies WHERE id = ?;", -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE);
}

int db_clear_cookies(void) {
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, "DELETE FROM cookies WHERE profile_id = ?;", -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, active_profile_id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE);
}

void db_export_cookies_to_file(int profile_id, const char* filepath) {
    FILE* f = fopen(filepath, "w");
    if (!f) return;
    
    fprintf(f, "# Netscape HTTP Cookie File\n");
    fprintf(f, "# Generated by MeetlookersBrowser\n");
    
    sqlite3_stmt* stmt;
    const char* sql = "SELECT domain, path, expiry, name, value, is_secure, same_site FROM cookies WHERE profile_id = ?;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, profile_id);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* domain = (const char*)sqlite3_column_text(stmt, 0);
            const char* path = (const char*)sqlite3_column_text(stmt, 1);
            long long expiry = sqlite3_column_int64(stmt, 2);
            const char* name = (const char*)sqlite3_column_text(stmt, 3);
            const char* value = (const char*)sqlite3_column_text(stmt, 4);
            int is_secure = sqlite3_column_int(stmt, 5);
            int same_site = sqlite3_column_int(stmt, 6); if (same_site < 1 || same_site > 3) same_site = 2;
            
            fprintf(f, "%s\t%s\t%s\t%s\t%lld\t%s\t%s\t%d\n",
                    domain ? domain : "",
                    "TRUE",
                    path ? path : "/",
                    is_secure ? "TRUE" : "FALSE",
                    expiry,
                    name ? name : "",
                    value ? value : "", same_site);
        }
        sqlite3_finalize(stmt);
    }
    fclose(f);
}

void db_import_cookies_from_file(int profile_id, const char* filepath) {
    FILE* f = fopen(filepath, "r");
    if (!f) return;
    
    char line[2048];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        
        int len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[len - 1] = '\0';
            len--;
        }
        
        char domain[256] = {0};
        char flag1[16] = {0};
        char path[256] = {0};
        char flag2[16] = {0};
        long long expiry = 0;
        char name[256] = {0};
        char value[1024] = {0};
        int same_site = 2;
        
        char* d = strtok(line, "\t");
        char* f1 = strtok(NULL, "\t");
        char* p = strtok(NULL, "\t");
        char* f2 = strtok(NULL, "\t");
        char* exp_str = strtok(NULL, "\t");
        char* n = strtok(NULL, "\t");
        char* v = strtok(NULL, "\t");
        char* ss = strtok(NULL, "\t"); if (ss) { int x = atoi(ss); if (x >= 1 && x <= 3) same_site = x; }
        
        if (d && f1 && p && f2 && exp_str && n) {
            strncpy(domain, d, 255);
            strncpy(path, p, 255);
            expiry = atoll(exp_str);
            strncpy(name, n, 255);
            if (v) {
                strncpy(value, v, 1023);
            } else {
                value[0] = '\0';
            }
            int is_secure = (strcmp(f2, "TRUE") == 0) ? 1 : 0;
            
            db_save_cookie(profile_id, domain, name, value, path, expiry, is_secure, same_site);
        }
    }
    fclose(f);
}

char* db_generate_cookies_html(void) {
    char* html = malloc(16384);
    strcpy(html, 
        "<h>Persistent Cookies</h>"
        "<p>List of stored cookies for the active profile.</p>"
        "<a href=\"localhost/home\">Back to Start Page</a> | "
        "<button onclick=\"Browser.clearCookies()\">Delete All Cookies</button><p></p>");

    sqlite3_stmt* stmt;
    const char* sql = "SELECT id, domain, name, value, path FROM cookies WHERE profile_id = ? LIMIT 100;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, active_profile_id);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            int id = sqlite3_column_int(stmt, 0);
            const char* domain = (const char*)sqlite3_column_text(stmt, 1);
            const char* name = (const char*)sqlite3_column_text(stmt, 2);
            const char* value = (const char*)sqlite3_column_text(stmt, 3);
            const char* path = (const char*)sqlite3_column_text(stmt, 4);

            char val_short[32];
            if (value && strlen(value) > 20) {
                strncpy(val_short, value, 17);
                strcpy(val_short + 17, "...");
            } else {
                strcpy(val_short, value ? value : "");
            }

            char buf[1024];
            snprintf(buf, sizeof(buf), 
                     "<p>Domain: <b>%s</b> | Name: <b>%s</b> | Val: <i>%s</i> | Path: %s "
                     "<button onclick=\"Browser.deleteCookie(%d)\">Delete</button></p>",
                     domain ? domain : "", name ? name : "", val_short, path ? path : "", id);
            strcat(html, buf);
        }
        sqlite3_finalize(stmt);
    }
    return html;
}

int db_save_personal_info(const char* key, const char* value) {
    sqlite3_stmt* stmt;
    const char* sql = "INSERT OR REPLACE INTO personal_info (profile_id, key, value) VALUES (?, ?, ?);";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    
    sqlite3_bind_int(stmt, 1, active_profile_id);
    sqlite3_bind_text(stmt, 2, key, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, value, -1, SQLITE_TRANSIENT);
    
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE);
}

char* db_get_personal_info(const char* key) {
    sqlite3_stmt* stmt;
    char* value = NULL;
    if (sqlite3_prepare_v2(db, "SELECT value FROM personal_info WHERE profile_id = ? AND key = ?;", -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, active_profile_id);
        sqlite3_bind_text(stmt, 2, key, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* val = (const char*)sqlite3_column_text(stmt, 0);
            if (val) value = strdup(val);
        }
        sqlite3_finalize(stmt);
    }
    return value;
}

/* ── localStorage: ключи хранятся построчно по (profile_id, origin) ── */
int db_ls_set(const char* origin, const char* key, const char* value) {
    sqlite3_stmt* stmt;
    const char* sql = "INSERT OR REPLACE INTO local_storage (profile_id, origin, key, value) VALUES (?, ?, ?, ?);";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int(stmt, 1, active_profile_id);
    sqlite3_bind_text(stmt, 2, origin, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, key, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, value, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE);
}

int db_ls_remove(const char* origin, const char* key) {
    sqlite3_stmt* stmt;
    const char* sql = "DELETE FROM local_storage WHERE profile_id = ? AND origin = ? AND key = ?;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int(stmt, 1, active_profile_id);
    sqlite3_bind_text(stmt, 2, origin, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, key, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE);
}

int db_ls_clear(const char* origin) {
    sqlite3_stmt* stmt;
    const char* sql = "DELETE FROM local_storage WHERE profile_id = ? AND origin = ?;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int(stmt, 1, active_profile_id);
    sqlite3_bind_text(stmt, 2, origin, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE);
}

/* Отдаёт всё хранилище источника разом как один JSON-объект {"key":"value",...} — так дешевле гидратировать Storage при открытии страницы,
   чем ходить в БД на каждый ключ. Строки экранируются вручную (JSON.stringify недоступен на этой стороне). */
static void json_esc_append(char** buf, size_t* cap, size_t* len, const char* s) {
    if (*len + 2 >= *cap) { *cap = (*cap + strlen(s) + 64) * 2; *buf = realloc(*buf, *cap); }
    (*buf)[(*len)++] = '"';
    for (const unsigned char* p = (const unsigned char*)(s ? s : ""); *p; p++) {
        if (*len + 8 >= *cap) { *cap *= 2; *buf = realloc(*buf, *cap); }
        if (*p == '"' || *p == '\\') { (*buf)[(*len)++] = '\\'; (*buf)[(*len)++] = (char)*p; }
        else if (*p == '\n') { (*buf)[(*len)++] = '\\'; (*buf)[(*len)++] = 'n'; }
        else if (*p == '\r') { (*buf)[(*len)++] = '\\'; (*buf)[(*len)++] = 'r'; }
        else if (*p == '\t') { (*buf)[(*len)++] = '\\'; (*buf)[(*len)++] = 't'; }
        else if (*p < 0x20) { char h[8]; snprintf(h, sizeof h, "\\u%04x", *p); memcpy(*buf + *len, h, 6); *len += 6; }
        else (*buf)[(*len)++] = (char)*p;
    }
    (*buf)[(*len)++] = '"';
}
char* db_ls_list_json(const char* origin) {
    size_t cap = 256, len = 0; char* buf = malloc(cap); if (!buf) return NULL;
    buf[len++] = '{';
    sqlite3_stmt* stmt; int first = 1;
    if (sqlite3_prepare_v2(db, "SELECT key, value FROM local_storage WHERE profile_id = ? AND origin = ?;", -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, active_profile_id);
        sqlite3_bind_text(stmt, 2, origin, -1, SQLITE_TRANSIENT);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* k = (const char*)sqlite3_column_text(stmt, 0);
            const char* v = (const char*)sqlite3_column_text(stmt, 1);
            if (!k) continue;
            if (!first) { if (len + 1 >= cap) { cap *= 2; buf = realloc(buf, cap); } buf[len++] = ','; }
            first = 0;
            json_esc_append(&buf, &cap, &len, k);
            if (len + 1 >= cap) { cap *= 2; buf = realloc(buf, cap); } buf[len++] = ':';
            json_esc_append(&buf, &cap, &len, v);
        }
        sqlite3_finalize(stmt);
    }
    if (len + 2 >= cap) { cap += 2; buf = realloc(buf, cap); }
    buf[len++] = '}'; buf[len] = 0;
    return buf;
}

/* ── IndexedDB: записи JSON, раздельные по профилю / origin / database / object store ── */
static void json_reserve(char** buf, size_t* cap, size_t need) {
    if (*cap >= need) return;
    size_t nc = *cap ? *cap : 256;
    while (nc < need) nc *= 2;
    char* nb = realloc(*buf, nc);
    if (!nb) return;
    *buf = nb; *cap = nc;
}
static void json_add_raw(char** b, size_t* cap, size_t* len, const char* v) {
    if (!v) v = "null";
    size_t n = strlen(v); json_reserve(b, cap, *len + n + 1);
    if (!*b || *cap < *len + n + 1) return;
    memcpy(*b + *len, v, n); *len += n; (*b)[*len] = 0;
}
char* db_idb_meta_list(const char* origin) {
    size_t cap = 512, len = 0; char* out = malloc(cap); if (!out) return NULL; out[0] = 0;
    json_add_raw(&out,&cap,&len,"["); int first = 1; sqlite3_stmt* st = NULL;
    if (sqlite3_prepare_v2(db,"SELECT name,version,stores_json FROM indexed_db_meta WHERE profile_id=? AND origin=? ORDER BY name;",-1,&st,NULL)==SQLITE_OK) {
        sqlite3_bind_int(st,1,active_profile_id); sqlite3_bind_text(st,2,origin,-1,SQLITE_TRANSIENT);
        while (sqlite3_step(st)==SQLITE_ROW) {
            const char* n=(const char*)sqlite3_column_text(st,0); const char* stores=(const char*)sqlite3_column_text(st,2); char ver[32]; snprintf(ver,sizeof ver,"%d",sqlite3_column_int(st,1));
            if (!first) json_add_raw(&out,&cap,&len,","); first=0;
            json_add_raw(&out,&cap,&len,"{\"name\":"); json_esc_append(&out,&cap,&len,n?n:"");
            json_add_raw(&out,&cap,&len,",\"version\":"); json_add_raw(&out,&cap,&len,ver);
            json_add_raw(&out,&cap,&len,",\"stores\":"); json_add_raw(&out,&cap,&len,stores?stores:"[]"); json_add_raw(&out,&cap,&len,"}");
        } sqlite3_finalize(st);
    }
    json_add_raw(&out,&cap,&len,"]"); return out;
}
char* db_idb_meta_get(const char* origin, const char* name) {
    sqlite3_stmt* st=NULL; char* out=NULL;
    if (sqlite3_prepare_v2(db,"SELECT version,stores_json FROM indexed_db_meta WHERE profile_id=? AND origin=? AND name=?;",-1,&st,NULL)==SQLITE_OK) {
        sqlite3_bind_int(st,1,active_profile_id); sqlite3_bind_text(st,2,origin,-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,3,name,-1,SQLITE_TRANSIENT);
        if (sqlite3_step(st)==SQLITE_ROW) { const char* stores=(const char*)sqlite3_column_text(st,1); size_t n=strlen(stores?stores:"[]")+80; out=malloc(n); if(out) snprintf(out,n,"{\"version\":%d,\"stores\":%s}",sqlite3_column_int(st,0),stores?stores:"[]"); }
        sqlite3_finalize(st);
    }
    return out;
}
int db_idb_meta_set(const char* origin,const char* name,int version,const char* stores_json) {
    sqlite3_stmt* st=NULL; const char* q="INSERT OR REPLACE INTO indexed_db_meta(profile_id,origin,name,version,stores_json) VALUES(?,?,?,?,?);";
    if(sqlite3_prepare_v2(db,q,-1,&st,NULL)!=SQLITE_OK)return 0;
    sqlite3_bind_int(st,1,active_profile_id);sqlite3_bind_text(st,2,origin,-1,SQLITE_TRANSIENT);sqlite3_bind_text(st,3,name,-1,SQLITE_TRANSIENT);sqlite3_bind_int(st,4,version);sqlite3_bind_text(st,5,stores_json?stores_json:"[]",-1,SQLITE_TRANSIENT);
    int ok=sqlite3_step(st)==SQLITE_DONE;sqlite3_finalize(st);return ok;
}
int db_idb_delete_database(const char* origin,const char* name) {
    sqlite3_stmt* st=NULL; int ok=1;
    if(sqlite3_prepare_v2(db,"DELETE FROM indexed_db_records WHERE profile_id=? AND origin=? AND db_name=?;",-1,&st,NULL)==SQLITE_OK){sqlite3_bind_int(st,1,active_profile_id);sqlite3_bind_text(st,2,origin,-1,SQLITE_TRANSIENT);sqlite3_bind_text(st,3,name,-1,SQLITE_TRANSIENT);ok=sqlite3_step(st)==SQLITE_DONE;sqlite3_finalize(st);} else ok=0;
    if(sqlite3_prepare_v2(db,"DELETE FROM indexed_db_meta WHERE profile_id=? AND origin=? AND name=?;",-1,&st,NULL)==SQLITE_OK){sqlite3_bind_int(st,1,active_profile_id);sqlite3_bind_text(st,2,origin,-1,SQLITE_TRANSIENT);sqlite3_bind_text(st,3,name,-1,SQLITE_TRANSIENT);ok=(sqlite3_step(st)==SQLITE_DONE)&&ok;sqlite3_finalize(st);} else ok=0;
    return ok;
}
char* db_idb_list(const char* origin,const char* name,const char* store) {
    size_t cap=512,len=0;char* out=malloc(cap);if(!out)return NULL;out[0]=0;json_add_raw(&out,&cap,&len,"[");int first=1;sqlite3_stmt* st=NULL;
    const char* q="SELECT key,value_json FROM indexed_db_records WHERE profile_id=? AND origin=? AND db_name=? AND store_name=?;";
    if(sqlite3_prepare_v2(db,q,-1,&st,NULL)==SQLITE_OK){sqlite3_bind_int(st,1,active_profile_id);sqlite3_bind_text(st,2,origin,-1,SQLITE_TRANSIENT);sqlite3_bind_text(st,3,name,-1,SQLITE_TRANSIENT);sqlite3_bind_text(st,4,store,-1,SQLITE_TRANSIENT);
        while(sqlite3_step(st)==SQLITE_ROW){const char* k=(const char*)sqlite3_column_text(st,0);const char* v=(const char*)sqlite3_column_text(st,1);if(!first)json_add_raw(&out,&cap,&len,",");first=0;json_add_raw(&out,&cap,&len,"{\"k\":");json_esc_append(&out,&cap,&len,k?k:"");json_add_raw(&out,&cap,&len,",\"v\":");json_add_raw(&out,&cap,&len,v?v:"null");json_add_raw(&out,&cap,&len,"}");}
        sqlite3_finalize(st);}
    json_add_raw(&out,&cap,&len,"]");return out;
}
char* db_idb_get(const char* origin,const char* name,const char* store,const char* key) {
    sqlite3_stmt* st=NULL;char* out=NULL;const char* q="SELECT value_json FROM indexed_db_records WHERE profile_id=? AND origin=? AND db_name=? AND store_name=? AND key=?;";
    if(sqlite3_prepare_v2(db,q,-1,&st,NULL)==SQLITE_OK){sqlite3_bind_int(st,1,active_profile_id);sqlite3_bind_text(st,2,origin,-1,SQLITE_TRANSIENT);sqlite3_bind_text(st,3,name,-1,SQLITE_TRANSIENT);sqlite3_bind_text(st,4,store,-1,SQLITE_TRANSIENT);sqlite3_bind_text(st,5,key,-1,SQLITE_TRANSIENT);if(sqlite3_step(st)==SQLITE_ROW){const char* v=(const char*)sqlite3_column_text(st,0);out=strdup(v?v:"null");}sqlite3_finalize(st);}return out;
}
int db_idb_put(const char* origin,const char* name,const char* store,const char* key,const char* value_json) {
    sqlite3_stmt* st=NULL;const char* q="INSERT OR REPLACE INTO indexed_db_records(profile_id,origin,db_name,store_name,key,value_json) VALUES(?,?,?,?,?,?);";
    if(sqlite3_prepare_v2(db,q,-1,&st,NULL)!=SQLITE_OK)return 0;sqlite3_bind_int(st,1,active_profile_id);sqlite3_bind_text(st,2,origin,-1,SQLITE_TRANSIENT);sqlite3_bind_text(st,3,name,-1,SQLITE_TRANSIENT);sqlite3_bind_text(st,4,store,-1,SQLITE_TRANSIENT);sqlite3_bind_text(st,5,key,-1,SQLITE_TRANSIENT);sqlite3_bind_text(st,6,value_json?value_json:"null",-1,SQLITE_TRANSIENT);int ok=sqlite3_step(st)==SQLITE_DONE;sqlite3_finalize(st);return ok;
}
int db_idb_remove(const char* origin,const char* name,const char* store,const char* key) {
    sqlite3_stmt* st=NULL;const char* q="DELETE FROM indexed_db_records WHERE profile_id=? AND origin=? AND db_name=? AND store_name=? AND key=?;";
    if(sqlite3_prepare_v2(db,q,-1,&st,NULL)!=SQLITE_OK)return 0;sqlite3_bind_int(st,1,active_profile_id);sqlite3_bind_text(st,2,origin,-1,SQLITE_TRANSIENT);sqlite3_bind_text(st,3,name,-1,SQLITE_TRANSIENT);sqlite3_bind_text(st,4,store,-1,SQLITE_TRANSIENT);sqlite3_bind_text(st,5,key,-1,SQLITE_TRANSIENT);int ok=sqlite3_step(st)==SQLITE_DONE;sqlite3_finalize(st);return ok;
}
int db_idb_clear(const char* origin,const char* name,const char* store) {
    sqlite3_stmt* st=NULL;const char* q="DELETE FROM indexed_db_records WHERE profile_id=? AND origin=? AND db_name=? AND store_name=?;";
    if(sqlite3_prepare_v2(db,q,-1,&st,NULL)!=SQLITE_OK)return 0;sqlite3_bind_int(st,1,active_profile_id);sqlite3_bind_text(st,2,origin,-1,SQLITE_TRANSIENT);sqlite3_bind_text(st,3,name,-1,SQLITE_TRANSIENT);sqlite3_bind_text(st,4,store,-1,SQLITE_TRANSIENT);int ok=sqlite3_step(st)==SQLITE_DONE;sqlite3_finalize(st);return ok;
}

/* ── Service Worker registrations ── */
char* db_sw_list(const char* origin) {
    size_t cap=512,len=0;char* out=malloc(cap);if(!out)return NULL;out[0]=0;json_add_raw(&out,&cap,&len,"[");int first=1;sqlite3_stmt* st=NULL;
    if(sqlite3_prepare_v2(db,"SELECT scope,script_url FROM service_workers WHERE profile_id=? AND origin=? ORDER BY scope;",-1,&st,NULL)==SQLITE_OK){sqlite3_bind_int(st,1,active_profile_id);sqlite3_bind_text(st,2,origin,-1,SQLITE_TRANSIENT);while(sqlite3_step(st)==SQLITE_ROW){const char* sc=(const char*)sqlite3_column_text(st,0);const char* su=(const char*)sqlite3_column_text(st,1);if(!first)json_add_raw(&out,&cap,&len,",");first=0;json_add_raw(&out,&cap,&len,"{\"scope\":");json_esc_append(&out,&cap,&len,sc?sc:"");json_add_raw(&out,&cap,&len,",\"scriptURL\":");json_esc_append(&out,&cap,&len,su?su:"");json_add_raw(&out,&cap,&len,"}");}sqlite3_finalize(st);}
    json_add_raw(&out,&cap,&len,"]");return out;
}
int db_sw_set(const char* origin,const char* scope,const char* script_url) {
    sqlite3_stmt* st=NULL;const char* q="INSERT OR REPLACE INTO service_workers(profile_id,origin,scope,script_url,updated_at) VALUES(?,?,?,?,strftime('%s','now'));";
    if(sqlite3_prepare_v2(db,q,-1,&st,NULL)!=SQLITE_OK)return 0;sqlite3_bind_int(st,1,active_profile_id);sqlite3_bind_text(st,2,origin,-1,SQLITE_TRANSIENT);sqlite3_bind_text(st,3,scope,-1,SQLITE_TRANSIENT);sqlite3_bind_text(st,4,script_url,-1,SQLITE_TRANSIENT);int ok=sqlite3_step(st)==SQLITE_DONE;sqlite3_finalize(st);return ok;
}
int db_sw_remove(const char* origin,const char* scope) {
    sqlite3_stmt* st=NULL;const char* q="DELETE FROM service_workers WHERE profile_id=? AND origin=? AND scope=?;";
    if(sqlite3_prepare_v2(db,q,-1,&st,NULL)!=SQLITE_OK)return 0;sqlite3_bind_int(st,1,active_profile_id);sqlite3_bind_text(st,2,origin,-1,SQLITE_TRANSIENT);sqlite3_bind_text(st,3,scope,-1,SQLITE_TRANSIENT);int ok=sqlite3_step(st)==SQLITE_DONE;sqlite3_finalize(st);return ok;
}

int db_add_download(const char* url, const char* path, const char* filename, long long total_size) {
    sqlite3_stmt* stmt;
    const char* sql = "INSERT INTO downloads (url, path, filename, total_size, downloaded_size, status, start_time) VALUES (?, ?, ?, ?, 0, 'starting', ?);";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, url, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, filename, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 4, total_size);
    sqlite3_bind_int64(stmt, 5, (long long)time(NULL));
    sqlite3_step(stmt);
    int id = (int)sqlite3_last_insert_rowid(db);
    sqlite3_finalize(stmt);
    return id;
}

void db_update_download_progress(int id, long long downloaded, const char* status) {
    sqlite3_stmt* stmt;
    const char* sql = "UPDATE downloads SET downloaded_size = ?, status = ? WHERE id = ?;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, downloaded);
        sqlite3_bind_text(stmt, 2, status, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 3, id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

int db_download_set_validators(int id, const char* etag, const char* last_modified, int accept_ranges) {
    if (id <= 0) return 0;
    sqlite3_stmt* st = NULL;
    const char* q = "UPDATE downloads SET etag=?, last_modified=?, accept_ranges=? WHERE id=?;";
    if (sqlite3_prepare_v2(db, q, -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(st, 1, etag ? etag : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, last_modified ? last_modified : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 3, accept_ranges ? 1 : 0);
    sqlite3_bind_int(st, 4, id);
    int ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db) == 1;
    sqlite3_finalize(st);
    return ok;
}

int db_download_update_response(int id, long long total_size, const char* etag,
                                const char* last_modified, int accept_ranges) {
    if (id <= 0) return 0;
    sqlite3_stmt* st = NULL;
    const char* q = "UPDATE downloads SET total_size=CASE WHEN ? >= 0 THEN ? ELSE total_size END, etag=?, last_modified=?, accept_ranges=? WHERE id=?;";
    if (sqlite3_prepare_v2(db, q, -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int64(st, 1, total_size);
    sqlite3_bind_int64(st, 2, total_size);
    sqlite3_bind_text(st, 3, etag ? etag : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, last_modified ? last_modified : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 5, accept_ranges ? 1 : 0);
    sqlite3_bind_int(st, 6, id);
    int ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db) == 1;
    sqlite3_finalize(st);
    return ok;
}

/* At startup no in-memory worker can still own these rows. Reconcile byte counts from
   disk before marking stale jobs interrupted, so a later resume never trusts stale DB sizes. */
void db_download_recover_stale(void) {
    sqlite3_stmt* read = NULL;
    if (sqlite3_prepare_v2(db, "SELECT id,path,total_size,status FROM downloads WHERE status IN ('starting','downloading','paused');", -1, &read, NULL) != SQLITE_OK) return;
    typedef struct { int id; char path[4096]; long long total; } Stale;
    Stale *rows = NULL; size_t count = 0, cap = 0;
    while (sqlite3_step(read) == SQLITE_ROW) {
        const char* path = (const char*)sqlite3_column_text(read, 1);
        if (!path || strlen(path) >= sizeof(((Stale*)0)->path)) continue;
        if (count == cap) { size_t nc = cap ? cap * 2 : 16; Stale* q = realloc(rows, nc * sizeof(*q)); if (!q) break; rows = q; cap = nc; }
        rows[count].id = sqlite3_column_int(read, 0);
        snprintf(rows[count].path, sizeof(rows[count].path), "%s", path);
        rows[count].total = sqlite3_column_int64(read, 2);
        count++;
    }
    sqlite3_finalize(read);
    for (size_t i = 0; i < count; ++i) {
        /* Import an atomically published response sidecar before status recovery. This covers
           a crash between receiving headers and the main process polling the worker. */
        char meta_path[128];
        snprintf(meta_path, sizeof(meta_path), "cache/download_%d.meta", rows[i].id);
        FILE* mf = fopen(meta_path, "rb");
        if (mf) {
            char total_line[64], range_line[16], etag[1024], modified[256];
            int parsed = fgets(total_line, sizeof(total_line), mf) && fgets(range_line, sizeof(range_line), mf) &&
                         fgets(etag, sizeof(etag), mf) && fgets(modified, sizeof(modified), mf);
            fclose(mf);
            if (parsed) {
                total_line[strcspn(total_line, "\r\n")] = 0; range_line[strcspn(range_line, "\r\n")] = 0;
                etag[strcspn(etag, "\r\n")] = 0; modified[strcspn(modified, "\r\n")] = 0;
                char* ep = NULL; errno = 0; long long total = strtoll(total_line, &ep, 10);
                if (!errno && ep && !*ep && total >= -1 && (!strcmp(range_line, "0") || !strcmp(range_line, "1")) &&
                    !strchr(etag, '\t') && !strchr(modified, '\t')) {
                    sqlite3_stmt* ms = NULL;
                    if (sqlite3_prepare_v2(db, "UPDATE downloads SET total_size=CASE WHEN ? >= 0 THEN ? ELSE total_size END,etag=?,last_modified=?,accept_ranges=? WHERE id=?;", -1, &ms, NULL) == SQLITE_OK) {
                        sqlite3_bind_int64(ms, 1, total); sqlite3_bind_int64(ms, 2, total);
                        sqlite3_bind_text(ms, 3, etag, -1, SQLITE_TRANSIENT); sqlite3_bind_text(ms, 4, modified, -1, SQLITE_TRANSIENT);
                        sqlite3_bind_int(ms, 5, atoi(range_line) == 1); sqlite3_bind_int(ms, 6, rows[i].id);
                        (void)sqlite3_step(ms); sqlite3_finalize(ms);
                        rows[i].total = total;
                        unlink(meta_path);
                    }
                }
            }
        }
        struct stat sb; long long bytes = 0; int have_file = stat(rows[i].path, &sb) == 0 && S_ISREG(sb.st_mode) && sb.st_size >= 0;
        if (have_file) bytes = (long long)sb.st_size;
        const char* state = (have_file && rows[i].total > 0 && bytes == rows[i].total) ? "complete" :
                            (have_file && bytes > 0 ? "interrupted" : "failed");
        sqlite3_stmt* upd = NULL;
        if (sqlite3_prepare_v2(db, "UPDATE downloads SET downloaded_size=?,status=? WHERE id=?;", -1, &upd, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(upd, 1, bytes); sqlite3_bind_text(upd, 2, state, -1, SQLITE_STATIC); sqlite3_bind_int(upd, 3, rows[i].id);
            (void)sqlite3_step(upd); sqlite3_finalize(upd);
        }
    }
    free(rows);
}

int db_download_get_resume_info(int id, DBDownloadResumeInfo* out) {
    if (id <= 0 || !out) return 0;
    memset(out, 0, sizeof(*out));
    sqlite3_stmt* st = NULL;
    const char* q = "SELECT url,path,downloaded_size,total_size,status,etag,last_modified,accept_ranges FROM downloads WHERE id=? LIMIT 1;";
    if (sqlite3_prepare_v2(db, q, -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int(st, 1, id);
    int ok = 0;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const char* url = (const char*)sqlite3_column_text(st, 0);
        const char* path = (const char*)sqlite3_column_text(st, 1);
        const char* status = (const char*)sqlite3_column_text(st, 4);
        const char* etag = (const char*)sqlite3_column_text(st, 5);
        const char* modified = (const char*)sqlite3_column_text(st, 6);
        long long downloaded = sqlite3_column_int64(st, 2);
        long long total = sqlite3_column_int64(st, 3);
        int ranges = sqlite3_column_int(st, 7);
        int interrupted = status && (!strcmp(status, "paused") || !strcmp(status, "interrupted") || !strcmp(status, "failed"));
        int validator = (etag && *etag && strncmp(etag, "W/", 2) != 0) || (modified && *modified);
        if (url && path && strlen(url) < sizeof(out->url) && strlen(path) < sizeof(out->path) &&
            downloaded > 0 && total >= 0 && downloaded <= total && interrupted && validator && ranges) {
            struct stat sb;
            if (stat(path, &sb) == 0 && S_ISREG(sb.st_mode) && (long long)sb.st_size == downloaded) {
                out->id = id;
                snprintf(out->url, sizeof(out->url), "%s", url);
                snprintf(out->path, sizeof(out->path), "%s", path);
                snprintf(out->etag, sizeof(out->etag), "%s", etag ? etag : "");
                snprintf(out->last_modified, sizeof(out->last_modified), "%s", modified ? modified : "");
                out->downloaded_size = downloaded; out->total_size = total; out->accept_ranges = ranges;
                ok = 1;
            }
        }
    }
    sqlite3_finalize(st);
    return ok;
}

static int downloads_html_append(char** out, size_t* len, size_t* cap, const char* s) {
    size_t n = strlen(s);
    if (n > (1024u * 1024u) - *len - 1) return 0;
    if (*len + n + 1 > *cap) {
        size_t nc = *cap ? *cap : 1024;
        while (nc < *len + n + 1) {
            if (nc > (1024u * 1024u) / 2) { nc = 1024u * 1024u; break; }
            nc *= 2;
        }
        char* tmp = realloc(*out, nc);
        if (!tmp) return 0;
        *out = tmp; *cap = nc;
    }
    memcpy(*out + *len, s, n); *len += n; (*out)[*len] = 0;
    return 1;
}

static int downloads_html_escape(char** out, size_t* len, size_t* cap, const char* s) {
    if (!s) s = "";
    for (const unsigned char* p = (const unsigned char*)s; *p; ++p) {
        const char* rep = NULL;
        switch (*p) {
            case '&': rep = "&amp;"; break;
            case '<': rep = "&lt;"; break;
            case '>': rep = "&gt;"; break;
            case '"': rep = "&quot;"; break;
            case '\'': rep = "&#39;"; break;
            default: break;
        }
        char one[2] = {(char)*p, 0};
        if (!downloads_html_append(out, len, cap, rep ? rep : one)) return 0;
    }
    return 1;
}

char* db_generate_downloads_html(void) {
    char* html = NULL; size_t len = 0, cap = 0;
    if (!downloads_html_append(&html, &len, &cap,
        "<!doctype html><meta charset=\"utf-8\"><title>Downloads</title>"
        "<h1>Downloads</h1><p>Downloaded files and current transfer state.</p>"
        "<style>.download{padding:8px 0}.download progress{width:min(480px,95%);height:1em}.download small{overflow-wrap:anywhere}</style>"
        "<a href=\"localhost/home\">Back to Start Page</a><hr>")) return html;

    sqlite3_stmt* stmt = NULL;
    const char* sql = "SELECT id, filename, url, total_size, downloaded_size, status, etag, last_modified, accept_ranges FROM downloads ORDER BY start_time DESC LIMIT 500;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* filename = (const char*)sqlite3_column_text(stmt, 1);
            const char* url = (const char*)sqlite3_column_text(stmt, 2);
            long long total = sqlite3_column_int64(stmt, 3);
            long long down = sqlite3_column_int64(stmt, 4);
            const char* status = (const char*)sqlite3_column_text(stmt, 5);
            int download_id = sqlite3_column_int(stmt, 0);
            char nums[192];
            if (!downloads_html_append(&html, &len, &cap, "<section class=\"download\"><p><strong>") ||
                !downloads_html_escape(&html, &len, &cap, filename) ||
                !downloads_html_append(&html, &len, &cap, "</strong> — ") ||
                !downloads_html_escape(&html, &len, &cap, status) ||
                !downloads_html_append(&html, &len, &cap, "</p><p>")) break;
            if (total < 0) snprintf(nums, sizeof nums, "%lld bytes downloaded", down < 0 ? 0 : down);
            else snprintf(nums, sizeof nums, "%lld / %lld bytes", down < 0 ? 0 : down, total);
            if (!downloads_html_append(&html, &len, &cap, nums)) break;
            if (total > 0) {
                char progress[160];
                long long safe_down = down < 0 ? 0 : (down > total ? total : down);
                snprintf(progress, sizeof progress, "</p><progress max=\"%lld\" value=\"%lld\"></progress><p>", total, safe_down);
                if (!downloads_html_append(&html, &len, &cap, progress)) break;
            }
            if (!downloads_html_append(&html, &len, &cap, "</p><small>") ||
                !downloads_html_escape(&html, &len, &cap, url) ||
                !downloads_html_append(&html, &len, &cap, "</small><p class=\"actions\">")) break;
            char action[256];
            if (status && (!strcmp(status, "downloading") || !strcmp(status, "starting"))) {
                snprintf(action, sizeof action, "<a href=\"browser://downloads?action=pause&amp;id=%d\">Pause</a> | <a href=\"browser://downloads?action=cancel&amp;id=%d\">Cancel</a>", download_id, download_id);
                if (!downloads_html_append(&html, &len, &cap, action)) break;
            } else if (status && !strcmp(status, "paused")) {
                snprintf(action, sizeof action, "<a href=\"browser://downloads?action=continue&amp;id=%d\">Continue</a> | <a href=\"browser://downloads?action=cancel&amp;id=%d\">Cancel</a>", download_id, download_id);
                if (!downloads_html_append(&html, &len, &cap, action)) break;
            } else if (status && (!strcmp(status, "interrupted") || !strcmp(status, "failed"))) {
                const char* etag = (const char*)sqlite3_column_text(stmt, 6);
                const char* modified = (const char*)sqlite3_column_text(stmt, 7);
                int ranges = sqlite3_column_int(stmt, 8);
                int strong_etag = etag && *etag && strncmp(etag, "W/", 2) != 0;
                if (ranges && (strong_etag || (modified && *modified))) {
                    snprintf(action, sizeof action, "<a href=\"browser://downloads?action=resume&amp;id=%d\">Resume</a>", download_id);
                    if (!downloads_html_append(&html, &len, &cap, action)) break;
                } else if (!downloads_html_append(&html, &len, &cap, "Resume unavailable (server validator not saved)")) break;
            }
            if (!downloads_html_append(&html, &len, &cap, "</p></section><hr>")) break;
        }
        sqlite3_finalize(stmt);
    }
    if (!html) return NULL;
    return html;
}
#include "browser_features.inc"

#else
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

DOMNode* find_dom_node_by_id_in_tree(DOMNode* head, const char* id) {
    DOMNode* curr = head;
    while (curr) {
        if (strcmp(curr->id, id) == 0) return curr;
        curr = curr->next;
    }
    return NULL;
}

int db_init(void) { return 1; }
void db_close(void) {}
int db_export_profile_backup(const char* p) { (void)p; return 0; }
int db_import_profile_backup(const char* p) { (void)p; return 0; }
int db_get_active_profile(char* name_out, int max_len) { if(name_out && max_len > 7) strcpy(name_out, "Default"); return 1; }
int db_set_active_profile(int profile_id) { (void)profile_id; return 1; }
int db_create_profile(const char* name) { (void)name; return 1; }
int db_delete_profile(int profile_id) { (void)profile_id; return 1; }
char* db_generate_profile_html(void) { return strdup("<p>Database support not compiled.</p>"); }
int db_add_history_entry(const char* url, const char* title) { (void)url; (void)title; return 1; }
int db_delete_history_entry(int id) { (void)id; return 1; }
int db_clear_history(void) { return 1; }
char* db_generate_history_html(void) { return strdup("<p>History support not compiled.</p>"); }
int db_save_cookie(int p, const char* d, const char* n, const char* v, const char* pa, long long e, int s, int ss) { (void)p;(void)d;(void)n;(void)v;(void)pa;(void)e;(void)s;(void)ss; return 1; }
int db_delete_cookie(int id) { (void)id; return 1; }
int db_clear_cookies(void) { return 1; }
void db_export_cookies_to_file(int p, const char* f) { (void)p;(void)f; }
void db_import_cookies_from_file(int p, const char* f) { (void)p;(void)f; }
char* db_generate_cookies_html(void) { return strdup("<p>Cookie support not compiled.</p>"); }
int db_ls_set(const char* o, const char* k, const char* v) { (void)o;(void)k;(void)v; return 0; }
int db_ls_remove(const char* o, const char* k) { (void)o;(void)k; return 0; }
int db_ls_clear(const char* o) { (void)o; return 0; }
char* db_ls_list_json(const char* o) { (void)o; return strdup("{}"); }
int db_save_personal_info(const char* k, const char* v) { (void)k;(void)v; return 1; }
char* db_get_personal_info(const char* k) { (void)k; return NULL; }
int db_bookmark_add(const char* u,const char* t){(void)u;(void)t;return 0;}
int db_bookmark_delete(int i){(void)i;return 0;}
int db_bookmark_delete_url(const char*u){(void)u;return 0;}
int db_bookmark_exists(const char*u){(void)u;return 0;}
char* db_generate_bookmarks_html(void){return strdup("<p>Database support not compiled.</p>");}
int db_save_session_tab(int s,const char*u,int a){(void)s;(void)u;(void)a;return 0;}
int db_session_count(void){return 0;}
int db_session_get_url(int s,char*o,size_t c,int*a){(void)s;if(o&&c)o[0]=0;if(a)*a=0;return 0;}
int db_session_clear_from(int s){(void)s;return 0;}
int db_password_save(const char*o,const char*u,const char*p){(void)o;(void)u;(void)p;return 0;}
int db_password_get_for_origin(const char*o,char*u,size_t uc,char*p,size_t pc){(void)o;if(u&&uc)u[0]=0;if(p&&pc)p[0]=0;return 0;}
int db_password_delete(int i){(void)i;return 0;}
char* db_generate_passwords_html(void){return strdup("<p>Password manager disabled.</p>");}

#endif
