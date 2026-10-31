/* Browser UI persistence helpers. Included from db/start.c inside USE_SQLITE. */
static char* db_feature_html_escape(const char* in) {
    if (!in) return strdup("");
    size_t n = strlen(in), cap = n * 6 + 1;
    char* out = malloc(cap); if (!out) return NULL;
    size_t j = 0;
    for (size_t i = 0; i < n; i++) {
        const char* r = NULL;
        switch (in[i]) {
            case '&': r = "&amp;"; break; case '<': r = "&lt;"; break;
            case '>': r = "&gt;"; break; case '"': r = "&quot;"; break;
            case '\'': r = "&#39;"; break;
            default: out[j++] = in[i]; continue;
        }
        size_t z = strlen(r); memcpy(out + j, r, z); j += z;
    }
    out[j] = 0; return out;
}

int db_bookmark_add(const char* url, const char* title) {
    if (!db || !url || !*url) return 0;
    sqlite3_stmt* st = NULL;
    const char* sql = "INSERT INTO bookmarks(profile_id,url,title,created_at) VALUES(?,?,?,?) ON CONFLICT(profile_id,url) DO UPDATE SET title=excluded.title";
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int(st, 1, active_profile_id); sqlite3_bind_text(st, 2, url, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, title ? title : url, -1, SQLITE_TRANSIENT); sqlite3_bind_int64(st, 4, (sqlite3_int64)time(NULL));
    int ok = sqlite3_step(st) == SQLITE_DONE; sqlite3_finalize(st); return ok;
}
int db_bookmark_delete(int id) {
    if (!db) return 0; sqlite3_stmt* st = NULL;
    if (sqlite3_prepare_v2(db, "DELETE FROM bookmarks WHERE profile_id=? AND id=?", -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int(st, 1, active_profile_id); sqlite3_bind_int(st, 2, id);
    int ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db) > 0; sqlite3_finalize(st); return ok;
}
int db_bookmark_delete_url(const char* url) {
    if (!db || !url) return 0; sqlite3_stmt* st = NULL;
    if (sqlite3_prepare_v2(db, "DELETE FROM bookmarks WHERE profile_id=? AND url=?", -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int(st, 1, active_profile_id); sqlite3_bind_text(st, 2, url, -1, SQLITE_TRANSIENT);
    int ok = sqlite3_step(st) == SQLITE_DONE; sqlite3_finalize(st); return ok;
}
int db_bookmark_exists(const char* url) {
    if (!db || !url) return 0; sqlite3_stmt* st = NULL; int yes = 0;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM bookmarks WHERE profile_id=? AND url=? LIMIT 1", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int(st, 1, active_profile_id); sqlite3_bind_text(st, 2, url, -1, SQLITE_TRANSIENT);
        yes = sqlite3_step(st) == SQLITE_ROW; sqlite3_finalize(st);
    }
    return yes;
}
char* db_generate_bookmarks_html(void) {
    size_t cap = 4096, used = 0; char* html = malloc(cap); if (!html) return NULL;
    used += (size_t)snprintf(html + used, cap - used, "<h>Bookmarks</h><p>Saved pages in this profile.</p><hr>");
    sqlite3_stmt* st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT id,url,title FROM bookmarks WHERE profile_id=? ORDER BY title COLLATE NOCASE", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int(st, 1, active_profile_id);
        while (sqlite3_step(st) == SQLITE_ROW) {
            int id = sqlite3_column_int(st, 0); const char* url = (const char*)sqlite3_column_text(st, 1); const char* title = (const char*)sqlite3_column_text(st, 2);
            char* eu = db_feature_html_escape(url), *et = db_feature_html_escape(title); if (!eu || !et) { free(eu); free(et); continue; }
            size_t need = strlen(eu) + strlen(et) + 160;
            if (used + need + 1 > cap) { while (used + need + 1 > cap) cap *= 2; char* p = realloc(html, cap); if (!p) { free(eu); free(et); break; } html = p; }
            used += (size_t)snprintf(html + used, cap - used, "<p><a href=\"%s\">%s</a> <button onclick=\"Browser.removeBookmark(%d)\">Remove</button></p>", eu, et, id);
            free(eu); free(et);
        }
        sqlite3_finalize(st);
    }
    return html;
}

int db_save_session_tab(int slot, const char* url, int is_active) {
    if (!db || slot < 0 || !url) return 0; sqlite3_stmt* st = NULL;
    const char* sql = "INSERT INTO browser_session(profile_id,slot,url,is_active) VALUES(?,?,?,?) ON CONFLICT(profile_id,slot) DO UPDATE SET url=excluded.url,is_active=excluded.is_active";
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int(st, 1, active_profile_id); sqlite3_bind_int(st, 2, slot); sqlite3_bind_text(st, 3, url, -1, SQLITE_TRANSIENT); sqlite3_bind_int(st, 4, !!is_active);
    int ok = sqlite3_step(st) == SQLITE_DONE; sqlite3_finalize(st); return ok;
}
int db_session_count(void) {
    if (!db) return 0; sqlite3_stmt* st = NULL; int n = 0;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM browser_session WHERE profile_id=?", -1, &st, NULL) == SQLITE_OK) { sqlite3_bind_int(st, 1, active_profile_id); if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0); sqlite3_finalize(st); }
    return n;
}
int db_session_get_url(int slot, char* out, size_t cap, int* is_active) {
    if (!db || !out || cap == 0) return 0; out[0] = 0; sqlite3_stmt* st = NULL; int ok = 0;
    if (sqlite3_prepare_v2(db, "SELECT url,is_active FROM browser_session WHERE profile_id=? AND slot=?", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int(st, 1, active_profile_id); sqlite3_bind_int(st, 2, slot);
        if (sqlite3_step(st) == SQLITE_ROW) { const char* url = (const char*)sqlite3_column_text(st, 0); snprintf(out, cap, "%s", url ? url : ""); if (is_active) *is_active = sqlite3_column_int(st, 1); ok = 1; }
        sqlite3_finalize(st);
    }
    return ok;
}
int db_session_clear_from(int first) {
    if (!db) return 0; sqlite3_stmt* st = NULL;
    if (sqlite3_prepare_v2(db, "DELETE FROM browser_session WHERE profile_id=? AND slot>=?", -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int(st, 1, active_profile_id); sqlite3_bind_int(st, 2, first); int ok = sqlite3_step(st) == SQLITE_DONE; sqlite3_finalize(st); return ok;
}

#ifdef HAVE_OPENSSL_VAULT
static unsigned char db_vault_key[32]; static int db_vault_state = 0;
static int db_vault_load_key(void) {
    if (db_vault_state) return db_vault_state > 0;
    const char* home = getenv("HOME"); if (!home || !*home) return 0;
    char cfg[1024], dir[1024], path[1200];
    int written = snprintf(cfg, sizeof cfg, "%s/.config", home);
    if (written < 0 || (size_t)written >= sizeof cfg) { db_vault_state = -1; return 0; }
    written = snprintf(dir, sizeof dir, "%s/meetlookers", cfg);
    if (written < 0 || (size_t)written >= sizeof dir) { db_vault_state = -1; return 0; }
    written = snprintf(path, sizeof path, "%s/password-vault.key", dir);
    if (written < 0 || (size_t)written >= sizeof path) { db_vault_state = -1; return 0; }
    if ((mkdir(cfg, 0700) < 0 && errno != EEXIST) ||
        (mkdir(dir, 0700) < 0 && errno != EEXIST)) { db_vault_state = -1; return 0; }
    int fd = open(path, O_RDONLY);
    if (fd >= 0) { ssize_t n = read(fd, db_vault_key, sizeof db_vault_key); close(fd); if (n == (ssize_t)sizeof db_vault_key) { db_vault_state = 1; return 1; } db_vault_state = -1; return 0; }
    if (errno != ENOENT || RAND_bytes(db_vault_key, sizeof db_vault_key) != 1) { db_vault_state = -1; return 0; }
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600); if (fd < 0) { db_vault_state = -1; return 0; }
    ssize_t n = write(fd, db_vault_key, sizeof db_vault_key); fsync(fd); close(fd);
    if (n != (ssize_t)sizeof db_vault_key) { unlink(path); db_vault_state = -1; return 0; } db_vault_state = 1; return 1;
}
static void db_vault_hex(const unsigned char* in, size_t n, char* out) { static const char h[] = "0123456789abcdef"; for (size_t i=0;i<n;i++) { out[i*2]=h[in[i]>>4]; out[i*2+1]=h[in[i]&15]; } out[n*2]=0; }
static int db_vault_unhex(const char* in, unsigned char* out, size_t n) {
    if (!in || strlen(in)!=n*2) return 0;
    for(size_t i=0;i<n;i++){char a=in[2*i],b=in[2*i+1];int x=(a>='0'&&a<='9')?a-'0':(a>='a'&&a<='f')?a-'a'+10:(a>='A'&&a<='F')?a-'A'+10:-1;int y=(b>='0'&&b<='9')?b-'0':(b>='a'&&b<='f')?b-'a'+10:(b>='A'&&b<='F')?b-'A'+10:-1;if(x<0||y<0)return 0;out[i]=(unsigned char)((x<<4)|y);}return 1;
}
static char* db_vault_encrypt(const char* plain) {
    if (!db_vault_load_key()) return NULL; size_t n = strlen(plain ? plain : ""); if (n > 8192) return NULL;
    unsigned char iv[12], tag[16]; unsigned char* cipher = malloc(n + 32); if (!cipher) return NULL; if (RAND_bytes(iv, sizeof iv) != 1) { free(cipher); return NULL; }
    EVP_CIPHER_CTX* c = EVP_CIPHER_CTX_new(); int l=0,total=0;
    int ok = c && EVP_EncryptInit_ex(c, EVP_aes_256_gcm(), NULL, NULL, NULL)==1 && EVP_CIPHER_CTX_ctrl(c,EVP_CTRL_GCM_SET_IVLEN,sizeof iv,NULL)==1 && EVP_EncryptInit_ex(c,NULL,NULL,db_vault_key,iv)==1 && EVP_EncryptUpdate(c,cipher,&l,(const unsigned char*)(plain?plain:""),(int)n)==1;
    total=l; if(ok)ok=EVP_EncryptFinal_ex(c,cipher+total,&l)==1; total+=l; if(ok)ok=EVP_CIPHER_CTX_ctrl(c,EVP_CTRL_GCM_GET_TAG,sizeof tag,tag)==1; EVP_CIPHER_CTX_free(c);
    if(!ok){free(cipher);return NULL;} char* hi=malloc(25),*ht=malloc(33),*hc=malloc((size_t)total*2+1),*out=malloc(64+(size_t)total*2); if(!hi||!ht||!hc||!out){free(hi);free(ht);free(hc);free(out);free(cipher);return NULL;}
    db_vault_hex(iv,12,hi);db_vault_hex(tag,16,ht);db_vault_hex(cipher,(size_t)total,hc);snprintf(out,64+(size_t)total*2,"v1:%s:%s:%s",hi,ht,hc);free(hi);free(ht);free(hc);free(cipher);return out;
}
static char* db_vault_decrypt(const char* enc) {
    if(!db_vault_load_key()||!enc||strncmp(enc,"v1:",3))return NULL;const char*a=enc+3,*b=strchr(a,':');if(!b)return NULL;const char*c=strchr(b+1,':');if(!c)return NULL;
    char ivs[25],tags[33];if((size_t)(b-a)!=24||(size_t)(c-(b+1))!=32)return NULL;memcpy(ivs,a,24);ivs[24]=0;memcpy(tags,b+1,32);tags[32]=0;size_t clen=strlen(c+1)/2;if(strlen(c+1)%2||clen>8192)return NULL;
    unsigned char iv[12],tag[16];if(!db_vault_unhex(ivs,iv,12)||!db_vault_unhex(tags,tag,16))return NULL;unsigned char*cipher=malloc(clen+1),*plain=malloc(clen+1);if(!cipher||!plain){free(cipher);free(plain);return NULL;}if(!db_vault_unhex(c+1,cipher,clen)){free(cipher);free(plain);return NULL;}
    EVP_CIPHER_CTX*ctx=EVP_CIPHER_CTX_new();int l=0,total=0;int ok=ctx&&EVP_DecryptInit_ex(ctx,EVP_aes_256_gcm(),NULL,NULL,NULL)==1&&EVP_CIPHER_CTX_ctrl(ctx,EVP_CTRL_GCM_SET_IVLEN,sizeof iv,NULL)==1&&EVP_DecryptInit_ex(ctx,NULL,NULL,db_vault_key,iv)==1&&EVP_DecryptUpdate(ctx,plain,&l,cipher,(int)clen)==1;total=l;if(ok)ok=EVP_CIPHER_CTX_ctrl(ctx,EVP_CTRL_GCM_SET_TAG,sizeof tag,tag)==1&&EVP_DecryptFinal_ex(ctx,plain+total,&l)==1;total+=l;EVP_CIPHER_CTX_free(ctx);free(cipher);if(!ok){memset(plain,0,clen);free(plain);return NULL;}plain[total]=0;return (char*)plain;
}
#else
static char* db_vault_encrypt(const char* p) { (void)p; return NULL; }
static char* db_vault_decrypt(const char* p) { (void)p; return NULL; }
#endif
int db_password_save(const char* origin,const char* username,const char* password) {
    if(!db||!origin||!username||!password||!*origin||!*username)return 0; char* enc=db_vault_encrypt(password);if(!enc)return 0;sqlite3_stmt*st=NULL;
    const char*sql="INSERT INTO saved_passwords(profile_id,origin,username,password_enc,created_at) VALUES(?,?,?,?,?) ON CONFLICT(profile_id,origin,username) DO UPDATE SET password_enc=excluded.password_enc,created_at=excluded.created_at";
    if(sqlite3_prepare_v2(db,sql,-1,&st,NULL)!=SQLITE_OK){free(enc);return 0;}sqlite3_bind_int(st,1,active_profile_id);sqlite3_bind_text(st,2,origin,-1,SQLITE_TRANSIENT);sqlite3_bind_text(st,3,username,-1,SQLITE_TRANSIENT);sqlite3_bind_text(st,4,enc,-1,SQLITE_TRANSIENT);sqlite3_bind_int64(st,5,(sqlite3_int64)time(NULL));int ok=sqlite3_step(st)==SQLITE_DONE;sqlite3_finalize(st);free(enc);return ok;
}
int db_password_get_for_origin(const char*origin,char*username,size_t ucap,char*password,size_t pcap) {
    if(username&&ucap)username[0]=0;if(password&&pcap)password[0]=0;if(!db||!origin||!username||!password||!ucap||!pcap)return 0;sqlite3_stmt*st=NULL;int ok=0;
    if(sqlite3_prepare_v2(db,"SELECT username,password_enc FROM saved_passwords WHERE profile_id=? AND origin=? ORDER BY created_at DESC LIMIT 1",-1,&st,NULL)==SQLITE_OK){sqlite3_bind_int(st,1,active_profile_id);sqlite3_bind_text(st,2,origin,-1,SQLITE_TRANSIENT);if(sqlite3_step(st)==SQLITE_ROW){const char*u=(const char*)sqlite3_column_text(st,0);const char*e=(const char*)sqlite3_column_text(st,1);char*p=db_vault_decrypt(e);if(p&&u){snprintf(username,ucap,"%s",u);snprintf(password,pcap,"%s",p);ok=1;}if(p){memset(p,0,strlen(p));free(p);}}sqlite3_finalize(st);}return ok;
}
int db_password_delete(int id){if(!db)return 0;sqlite3_stmt*st=NULL;if(sqlite3_prepare_v2(db,"DELETE FROM saved_passwords WHERE profile_id=? AND id=?",-1,&st,NULL)!=SQLITE_OK)return 0;sqlite3_bind_int(st,1,active_profile_id);sqlite3_bind_int(st,2,id);int ok=sqlite3_step(st)==SQLITE_DONE&&sqlite3_changes(db)>0;sqlite3_finalize(st);return ok;}
char* db_generate_passwords_html(void) {
    size_t cap=4096,used=0;char*html=malloc(cap);if(!html)return NULL;used+=(size_t)snprintf(html+used,cap-used,"<h>Password manager</h><p>Saved credentials are AES-256-GCM encrypted when OpenSSL support is compiled in.</p><p>Fill a login form, then use the browser menu → Save current login.</p><hr>");sqlite3_stmt*st=NULL;
    if(sqlite3_prepare_v2(db,"SELECT id,origin,username FROM saved_passwords WHERE profile_id=? ORDER BY origin,username",-1,&st,NULL)==SQLITE_OK){sqlite3_bind_int(st,1,active_profile_id);while(sqlite3_step(st)==SQLITE_ROW){int id=sqlite3_column_int(st,0);const char*o=(const char*)sqlite3_column_text(st,1);const char*u=(const char*)sqlite3_column_text(st,2);char*eo=db_feature_html_escape(o),*eu=db_feature_html_escape(u);size_t need=strlen(eo?eo:"")+strlen(eu?eu:"")+128;if(used+need+1>cap){while(used+need+1>cap)cap*=2;char*q=realloc(html,cap);if(!q){free(eo);free(eu);break;}html=q;}used+=(size_t)snprintf(html+used,cap-used,"<p><b>%s</b> — %s <button onclick=\"Browser.removePassword(%d)\">Remove</button></p>",eo?eo:"",eu?eu:"",id);free(eo);free(eu);}sqlite3_finalize(st);}return html;
}
