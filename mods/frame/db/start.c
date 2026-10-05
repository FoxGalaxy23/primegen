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

    const char* sql_downloads =
        "CREATE TABLE IF NOT EXISTS downloads ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  url TEXT,"
        "  path TEXT,"
        "  filename TEXT,"
        "  total_size INTEGER,"
        "  downloaded_size INTEGER,"
        "  status TEXT,"
        "  start_time INTEGER"
        ");";

    sqlite3_exec(db, sql_profiles, NULL, NULL, NULL);
    sqlite3_exec(db, sql_history, NULL, NULL, NULL);
    sqlite3_exec(db, sql_cookies, NULL, NULL, NULL);
    sqlite3_exec(db, sql_personal, NULL, NULL, NULL);
    sqlite3_exec(db, sql_local_storage, NULL, NULL, NULL);
    sqlite3_exec(db, sql_downloads, NULL, NULL, NULL);

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

int db_save_cookie(int profile_id, const char* domain, const char* name, const char* value, const char* path, long long expiry, int is_secure) {
    sqlite3_stmt* stmt;
    const char* sql = "INSERT OR REPLACE INTO cookies (profile_id, domain, name, value, path, expiry, is_secure) VALUES (?, ?, ?, ?, ?, ?, ?);";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    
    sqlite3_bind_int(stmt, 1, profile_id);
    sqlite3_bind_text(stmt, 2, domain, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, value, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, path ? path : "/", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 6, expiry);
    sqlite3_bind_int(stmt, 7, is_secure);
    
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
    const char* sql = "SELECT domain, path, expiry, name, value, is_secure FROM cookies WHERE profile_id = ?;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, profile_id);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* domain = (const char*)sqlite3_column_text(stmt, 0);
            const char* path = (const char*)sqlite3_column_text(stmt, 1);
            long long expiry = sqlite3_column_int64(stmt, 2);
            const char* name = (const char*)sqlite3_column_text(stmt, 3);
            const char* value = (const char*)sqlite3_column_text(stmt, 4);
            int is_secure = sqlite3_column_int(stmt, 5);
            
            fprintf(f, "%s\t%s\t%s\t%s\t%lld\t%s\t%s\n",
                    domain ? domain : "",
                    "TRUE",
                    path ? path : "/",
                    is_secure ? "TRUE" : "FALSE",
                    expiry,
                    name ? name : "",
                    value ? value : "");
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
        
        char* d = strtok(line, "\t");
        char* f1 = strtok(NULL, "\t");
        char* p = strtok(NULL, "\t");
        char* f2 = strtok(NULL, "\t");
        char* exp_str = strtok(NULL, "\t");
        char* n = strtok(NULL, "\t");
        char* v = strtok(NULL, "\t");
        
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
            
            db_save_cookie(profile_id, domain, name, value, path, expiry, is_secure);
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

char* db_generate_downloads_html(void) {
    char* html = malloc(16384);
    strcpy(html, "<h>Downloads</h><p>List of downloaded files.</p><a href=\"localhost/home\">Back to Start Page</a><p></p>");

    sqlite3_stmt* stmt;
    const char* sql = "SELECT id, filename, url, total_size, downloaded_size, status FROM downloads ORDER BY start_time DESC;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* filename = (const char*)sqlite3_column_text(stmt, 1);
            const char* url = (const char*)sqlite3_column_text(stmt, 2);
            long long total = sqlite3_column_int64(stmt, 3);
            long long down = sqlite3_column_int64(stmt, 4);
            const char* status = (const char*)sqlite3_column_text(stmt, 5);

            char buf[1024];
            snprintf(buf, sizeof(buf),
                     "<p><b>%s</b> (%s) - %lld/%lld bytes [%s] <br><small>%s</small></p><hr>",
                     filename, status, down, total, (total > 0 ? "..." : "???"), url);
            strcat(html, buf);
        }
        sqlite3_finalize(stmt);
    }
    return html;
}
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
int db_get_active_profile(char* name_out, int max_len) { if(name_out && max_len > 7) strcpy(name_out, "Default"); return 1; }
int db_set_active_profile(int profile_id) { (void)profile_id; return 1; }
int db_create_profile(const char* name) { (void)name; return 1; }
int db_delete_profile(int profile_id) { (void)profile_id; return 1; }
char* db_generate_profile_html(void) { return strdup("<p>Database support not compiled.</p>"); }
int db_add_history_entry(const char* url, const char* title) { (void)url; (void)title; return 1; }
int db_delete_history_entry(int id) { (void)id; return 1; }
int db_clear_history(void) { return 1; }
char* db_generate_history_html(void) { return strdup("<p>History support not compiled.</p>"); }
int db_save_cookie(int p, const char* d, const char* n, const char* v, const char* pa, long long e, int s) { (void)p;(void)d;(void)n;(void)v;(void)pa;(void)e;(void)s; return 1; }
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
#endif
