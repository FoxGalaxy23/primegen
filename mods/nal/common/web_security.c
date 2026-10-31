#define _POSIX_C_SOURCE 200809L
#include "web_security.h"
#include "idna.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <time.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <direct.h>
#include <process.h>
#define getpid _getpid
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#define strtok_r strtok_s
#define MLB_MKDIR(p) _mkdir(p)
#else
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>
#include <arpa/inet.h>
#define MLB_MKDIR(p) mkdir((p), 0700)
#endif

#define CSP_MAX_POLICIES 16
#define CSP_MAX_LEN 4096
static char g_doc_url[8192];
static char g_csp[CSP_MAX_POLICIES][CSP_MAX_LEN];
static int g_csp_n;
static int g_doc_url_invalid;

static uint64_t fnv64(const char *s) { uint64_t h=1469598103934665603ULL; for(;*s;s++){h^=(unsigned char)*s;h*=1099511628211ULL;} return h; }
static void trim_line(char *s) { size_t n=strlen(s); while(n&&(s[n-1]=='\r'||s[n-1]=='\n'||isspace((unsigned char)s[n-1])))s[--n]=0; while(*s&&isspace((unsigned char)*s))memmove(s,s+1,strlen(s)); }
static void security_dir(void) { MLB_MKDIR("cache"); MLB_MKDIR("cache/security"); }
static void csp_path(const char *url,char *out,size_t cap) { snprintf(out,cap,"cache/security/csp_%016llx",(unsigned long long)fnv64(url?url:"")); }
static int doc_key(const char *s, char *out, size_t cap) {
    if (!out || !cap) return 0;
    if (!s) { out[0] = 0; return 1; }
    size_t n = strcspn(s, "#");
    if (n >= cap) { out[0] = 0; return 0; }
    memcpy(out, s, n); out[n] = 0;
    return 1;
}
static void csp_load_file(const char *url) {
    char path[128]; csp_path(url,path,sizeof path); FILE *f=fopen(path,"r"); if(!f)return;
    char key[8192]; if(!fgets(key,sizeof key,f)){fclose(f);return;} trim_line(key);
    char want[8192]; if(!doc_key(url,want,sizeof want) || strcmp(key,want)){fclose(f);return;}
    while(g_csp_n<CSP_MAX_POLICIES&&fgets(g_csp[g_csp_n],CSP_MAX_LEN,f)){trim_line(g_csp[g_csp_n]);if(g_csp[g_csp_n][0])g_csp_n++;}
    fclose(f);
}
void websec_set_document_url(const char *url) {
    char next[8192];
    if (!doc_key(url, next, sizeof next)) {
        /* An overlong document URL must not collapse to a truncated CSP identity. */
        g_doc_url_invalid = 1; g_doc_url[0] = 0; g_csp_n = 0; memset(g_csp, 0, sizeof g_csp);
        return;
    }
    if (!g_doc_url_invalid && !strcmp(next, g_doc_url)) return;
    g_doc_url_invalid = 0;
    memcpy(g_doc_url, next, strlen(next) + 1);
    g_csp_n = 0; memset(g_csp, 0, sizeof g_csp);
    if (g_doc_url[0]) csp_load_file(g_doc_url);
}
int websec_is_document_url(const char *url) {
    char a[8192], b[8192];
    if (g_doc_url_invalid || !doc_key(url, a, sizeof a) || !doc_key(g_doc_url, b, sizeof b)) return 0;
    return a[0] && b[0] && !strcmp(a, b);
}
void websec_set_response(const char *document_url,const char *headers) {
    websec_set_document_url(document_url);g_csp_n=0;memset(g_csp,0,sizeof g_csp);
    if (g_doc_url_invalid) return;
    if(headers){
        const char *p=headers;
        while(*p&&g_csp_n<CSP_MAX_POLICIES){const char *e=strchr(p,'\n');size_t n=e?(size_t)(e-p):strlen(p);
            if(n>24&&n<8192&&!strncasecmp(p,"Content-Security-Policy:",24)){
                char val[8192];size_t off=24;while(off<n&&isspace((unsigned char)p[off]))off++;size_t vl=n-off;if(vl>=sizeof val)vl=sizeof val-1;memcpy(val,p+off,vl);val[vl]=0;
                /* Multiple CSP policies are comma-separated and must all be satisfied. */
                char *save=NULL;for(char *part=strtok_r(val,",",&save);part&&g_csp_n<CSP_MAX_POLICIES;part=strtok_r(NULL,",",&save)){trim_line(part);if(*part){snprintf(g_csp[g_csp_n++],CSP_MAX_LEN,"%s",part);}}
            }
            if(!e)break;p=e+1;
        }
    }
    security_dir();char path[128],tmp[160],key[8192];csp_path(g_doc_url,path,sizeof path);snprintf(tmp,sizeof tmp,"%s.%ld.tmp",path,(long)getpid());if(!doc_key(g_doc_url,key,sizeof key))return;
    if(!g_csp_n){remove(path);return;}FILE*f=fopen(tmp,"w");if(!f)return;fprintf(f,"%s\n",key);for(int i=0;i<g_csp_n;i++)fprintf(f,"%s\n",g_csp[i]);if(fclose(f)==0)rename(tmp,path);else remove(tmp);
}

typedef struct {char scheme[16],host[256],path[1024];int port,has_port,valid;} UrlBits;
static int url_bits(const char *url,UrlBits *u) {
    memset(u,0,sizeof *u);if(!url)return 0;const char *sep=strstr(url,"://");if(!sep)return 0;size_t sn=(size_t)(sep-url);if(!sn||sn>=sizeof u->scheme)return 0;for(size_t i=0;i<sn;i++)u->scheme[i]=(char)tolower((unsigned char)url[i]);u->scheme[sn]=0;
    const char *a=sep+3,*end=a+strcspn(a,"/?#");const char *at=NULL;for(const char*p=a;p<end;p++)if(*p=='@')at=p;if(at)a=at+1;
    const char *he=end,*portp=NULL;
    if(a<end&&*a=='['){const char *cl=memchr(a,']',(size_t)(end-a));if(!cl)return 0;size_t hn=(size_t)(cl-a-1);if(!hn||hn>=sizeof u->host)return 0;memcpy(u->host,a+1,hn);u->host[hn]=0;if(cl+1<end&&cl[1]==':')portp=cl+2;else if(cl+1<end)return 0;}
    else {const char *colon=NULL;for(const char*p=a;p<end;p++)if(*p==':')colon=p; if(colon){he=colon;portp=colon+1;}size_t hn=(size_t)(he-a);if(!hn||hn>=sizeof u->host)return 0;memcpy(u->host,a,hn);u->host[hn]=0;}
    if(portp){char *ep=NULL;long n=strtol(portp,&ep,10);if(ep!=end||n<1||n>65535)return 0;u->port=(int)n;u->has_port=1;}else u->port=!strcmp(u->scheme,"https")||!strcmp(u->scheme,"wss")?443:(!strcmp(u->scheme,"http")||!strcmp(u->scheme,"ws")?80:0);
    if(u->host[0]&&u->host[0]!='['){char ascii[256];if(nidna_to_ascii(u->host,ascii,sizeof ascii))snprintf(u->host,sizeof u->host,"%s",ascii);else {for(char*p=u->host;*p;p++)*p=(char)tolower((unsigned char)*p);}}
    if(*end=='/')snprintf(u->path,sizeof u->path,"%.*s",(int)strcspn(end,"?#"),end);else snprintf(u->path,sizeof u->path,"/");u->valid=1;return 1;
}
static int default_port(const UrlBits*u){return (!strcmp(u->scheme,"http")||!strcmp(u->scheme,"ws"))?80:(!strcmp(u->scheme,"https")||!strcmp(u->scheme,"wss"))?443:0;}
static int scheme_self_match(const UrlBits*a,const UrlBits*b) {
    if (!strcmp(a->scheme,b->scheme)) return 1;
    if ((!strcmp(a->scheme,"https") && !strcmp(b->scheme,"wss")) || (!strcmp(a->scheme,"wss") && !strcmp(b->scheme,"https"))) return 1;
    if ((!strcmp(a->scheme,"http") && !strcmp(b->scheme,"ws")) || (!strcmp(a->scheme,"ws") && !strcmp(b->scheme,"http"))) return 1;
    return 0;
}
static int origin_equal(const UrlBits*a,const UrlBits*b){return a->valid&&b->valid&&scheme_self_match(a,b)&&!strcasecmp(a->host,b->host)&&a->port==b->port;}
static const char *directive_for(int kind) {
    switch(kind){case NAL_RESOURCE_SCRIPT:return "script-src";case NAL_RESOURCE_STYLE:return "style-src";case NAL_RESOURCE_IMAGE:return "img-src";case NAL_RESOURCE_FONT:return "font-src";case NAL_RESOURCE_CONNECT:return "connect-src";case NAL_RESOURCE_WORKER:return "worker-src";default:return "default-src";}
}
static int token_eq(const char *a,const char *b){return !strcasecmp(a,b);}
/* Returns a pointer to the first source token list for directive, applying CSP fallback rules. */
static int get_sources(const char *policy,int kind,char *out,size_t cap) {
    const char *wanted=directive_for(kind);char fallback[4][24];int nf=0;
    if(kind==NAL_RESOURCE_SCRIPT){strcpy(fallback[nf++],"script-src-elem");strcpy(fallback[nf++],"script-src");strcpy(fallback[nf++],"default-src");}
    else if(kind==NAL_RESOURCE_STYLE){strcpy(fallback[nf++],"style-src-elem");strcpy(fallback[nf++],"style-src");strcpy(fallback[nf++],"default-src");}
    else if(kind==NAL_RESOURCE_WORKER){strcpy(fallback[nf++],"worker-src");strcpy(fallback[nf++],"child-src");strcpy(fallback[nf++],"script-src");strcpy(fallback[nf++],"default-src");}
    else {snprintf(fallback[nf++],sizeof fallback[0],"%s",wanted);strcpy(fallback[nf++],"default-src");}
    for(int priority=0;priority<nf;priority++){
        char copy[CSP_MAX_LEN];snprintf(copy,sizeof copy,"%s",policy);char *sv=NULL;
        for(char*dir=strtok_r(copy,";",&sv);dir;dir=strtok_r(NULL,";",&sv)){
            while(isspace((unsigned char)*dir))dir++;char *sp=dir;while(*sp&&!isspace((unsigned char)*sp))sp++;char saved=*sp;*sp=0;
            int match=token_eq(dir,fallback[priority]);*sp=saved;
            if(match){while(isspace((unsigned char)*sp))sp++;snprintf(out,cap,"%s",sp);return 1;}
        }
    }
    return 0;
}
static int source_match(const char *raw,const char *url,const char *doc_url) {
    char src[512];
    snprintf(src,sizeof src,"%s",raw);
    while(*src&&isspace((unsigned char)*src))memmove(src,src+1,strlen(src));
    size_t sl=strlen(src);while(sl&&isspace((unsigned char)src[sl-1]))src[--sl]=0;
    if(!sl)return 0;
    if(!strcmp(src,"*"))return !strncasecmp(url,"http://",7)||!strncasecmp(url,"https://",8)||!strncasecmp(url,"ws://",5)||!strncasecmp(url,"wss://",6);
    if(src[0]=='\''){
        if(token_eq(src,"'self'")){UrlBits a,b;return url_bits(url,&a)&&url_bits(doc_url,&b)&&origin_equal(&a,&b);}
        if(token_eq(src,"'none'"))return 0;
        return 0;
    }
    if(!strchr(src,'/')&&src[strlen(src)-1]==':'){
        char sc[20];size_t n=strlen(src)-1;if(n>=sizeof sc)return 0;memcpy(sc,src,n);sc[n]=0;
        UrlBits u;if(!url_bits(url,&u))return 0;return token_eq(sc,u.scheme);
    }
    UrlBits u,d;if(!url_bits(url,&u)||!url_bits(doc_url,&d))return 0;
    char *authority=src;char expected_scheme[16]="";char *sep=strstr(src,"://");
    if(sep){*sep=0;if(strlen(src)>=sizeof expected_scheme)return 0;snprintf(expected_scheme,sizeof expected_scheme,"%s",src);for(char*p=expected_scheme;*p;p++)*p=(char)tolower((unsigned char)*p);authority=sep+3;}
    else if (!scheme_self_match(&u,&d)) return 0;

    char pathpart[256]="";char *path=NULL;
    if(authority[0]=='['){char *cl=strchr(authority,']');if(!cl)return 0;path=strchr(cl+1,'/');if(cl[1]&&cl[1]!=':'&&cl[1]!='/')return 0;}
    else path=strchr(authority,'/');
    if(path){if(strlen(path)>=sizeof pathpart)return 0;strcpy(pathpart,path);*path=0;}

    int wildcard=0,port=0;
    if(authority[0]=='['){
        char *cl=strchr(authority,']');if(!cl)return 0;
        if(cl[1]==':'){
            char *ps=cl+2;if(!strcmp(ps,"*"))port=-1;else {char*e=NULL;long z=strtol(ps,&e,10);if(e==ps||*e||z<1||z>65535)return 0;port=(int)z;}
            cl[1]=0;
        } else if(cl[1]) return 0;
    } else {
        if(authority[0]=='*'&&authority[1]=='.'){wildcard=1;authority+=2;}
        char *colon=strrchr(authority,':');
        if(colon){*colon=0;if(!strcmp(colon+1,"*"))port=-1;else {char*e=NULL;long z=strtol(colon+1,&e,10);if(e==colon+1||*e||z<1||z>65535)return 0;port=(int)z;}}
    }
    if(expected_scheme[0]&&strcmp(expected_scheme,u.scheme))return 0;
    char host[256];
    if(authority[0]=='['){size_t hn=strlen(authority);if(hn<3||authority[hn-1]!=']')return 0;hn-=2;if(hn>=sizeof host)return 0;memcpy(host,authority+1,hn);host[hn]=0;}
    else if(!nidna_to_ascii(authority,host,sizeof host)){size_t hn=strlen(authority);if(hn>=sizeof host)return 0;memcpy(host,authority,hn+1);for(char*p=host;*p;p++)*p=(char)tolower((unsigned char)*p);}
    int hm=wildcard?(strlen(u.host)>strlen(host)&&u.host[strlen(u.host)-strlen(host)-1]=='.'&&!strcasecmp(u.host+strlen(u.host)-strlen(host),host)):!strcasecmp(u.host,host);
    if(!hm)return 0;
    if(port>0&&u.port!=port)return 0;
    if(port==0 && u.port!=default_port(&u))return 0;
    if(pathpart[0]){size_t pl=strlen(pathpart);if(pathpart[pl-1]=='/'){if(strncmp(u.path,pathpart,pl))return 0;}else if(strcmp(u.path,pathpart))return 0;}
    return 1;
}
static int policy_allows(const char *policy,const char *url,int kind) {
    char sources[2048];if(!get_sources(policy,kind,sources,sizeof sources))return 1;
    char *sv=NULL;for(char*t=strtok_r(sources," \t\r\n",&sv);t;t=strtok_r(NULL," \t\r\n",&sv)){if(source_match(t,url,g_doc_url))return 1;}
    return 0;
}
int websec_allows(const char *url,int resource_kind) {
    if(!url||!url[0]||resource_kind==NAL_RESOURCE_DOCUMENT)return 1;
    if(g_doc_url_invalid)return 0;
    if(!g_doc_url[0]||!g_csp_n)return 1;
    if(!strncasecmp(url,"data:",5)){
        for(int i=0;i<g_csp_n;i++){char src[2048];if(get_sources(g_csp[i],resource_kind,src,sizeof src)){int ok=0;char*s=NULL;for(char*t=strtok_r(src," \t\r\n",&s);t;t=strtok_r(NULL," \t\r\n",&s))if(!strcasecmp(t,"data:"))ok=1;if(!ok)return 0;}}return 1;
    }
    for(int i=0;i<g_csp_n;i++)if(!policy_allows(g_csp[i],url,resource_kind))return 0;
    return 1;
}
int websec_allows_inline(int resource_kind,const char *nonce) {
    if(g_doc_url_invalid)return 0;
    if(!g_doc_url[0]||!g_csp_n)return 1;
    for(int i=0;i<g_csp_n;i++){
        char sources[2048];if(!get_sources(g_csp[i],resource_kind,sources,sizeof sources))continue;
        int has_nonce_hash=0,nonce_ok=0,unsafe_inline=0;char *sv=NULL;
        for(char*t=strtok_r(sources," \t\r\n",&sv);t;t=strtok_r(NULL," \t\r\n",&sv)){
            if(!strncasecmp(t,"'nonce-",7)||!strncasecmp(t,"'sha256-",8)||!strncasecmp(t,"'sha384-",8)||!strncasecmp(t,"'sha512-",8))has_nonce_hash=1;
            if(nonce&&nonce[0]&&strlen(t)==strlen(nonce)+8&&!strncasecmp(t,"'nonce-",7)&&!strncmp(t+7,nonce,strlen(nonce))&&t[7+strlen(nonce)]=='\'')nonce_ok=1;
            if(!strcasecmp(t,"'unsafe-inline'"))unsafe_inline=1;
        }
        if(has_nonce_hash){if(!nonce_ok)return 0;}else if(!unsafe_inline)return 0;
    }
    return 1;
}

static int ip_or_localhost(const char *host) {
    if(!host||!host[0]||!strcasecmp(host,"localhost")||!strcasecmp(host,"localhost."))return 1;
    if(strchr(host,':'))return 1;struct in_addr a;
#ifdef _WIN32
    (void)a; /* WinHTTP handles IP literals; reject dotted numeric hosts conservatively. */
    int dots=0,all=1;for(const char*p=host;*p;p++){if(*p=='.')dots++;else if(!isdigit((unsigned char)*p))all=0;}return all&&dots==3;
#else
    return inet_pton(AF_INET,host,&a)==1;
#endif
}
static int read_hsts_all(FILE *f,const char *host,int64_t now,int *sub) {
    char line[512];int matched=0;*sub=0;
    while(fgets(line,sizeof line,f)){char h[256];long long exp=0;int incl=0;if(sscanf(line,"%255s %lld %d",h,&exp,&incl)!=3)continue;if(exp<=now)continue;
        if(!strcasecmp(host,h)){matched=1;*sub=1;}
        else if(incl){size_t hl=strlen(host),dl=strlen(h);if(hl>dl&&host[hl-dl-1]=='.'&&!strcasecmp(host+hl-dl,h)){matched=1;*sub=1;}}
    }return matched;
}
static int hsts_match(const char *host) {
    security_dir();
#ifndef _WIN32
    int lk=open("cache/security/hsts.lock",O_CREAT|O_RDWR,0600);if(lk>=0)flock(lk,LOCK_SH);
#endif
    FILE*f=fopen("cache/security/hsts.txt","r");int sub=0,ok=0;if(f){ok=read_hsts_all(f,host,(int64_t)time(NULL),&sub);fclose(f);}
#ifndef _WIN32
    if(lk>=0){flock(lk,LOCK_UN);close(lk);}
#endif
    return ok;
}
int websec_hsts_upgrade_url(const char *url,char *out,size_t cap) {
    if(!url||strncasecmp(url,"http://",7)||!out||cap==0)return 0;
    const char *a=url+7,*end=a+strcspn(a,"/?#"),*at=NULL;for(const char*p=a;p<end;p++)if(*p=='@')at=p;if(at)a=at+1;
    char host[256],ascii[256];const char *he=end;int bracketed=0;
    if(a<end&&*a=='['){const char*cl=memchr(a,']',(size_t)(end-a));if(!cl)return 0;size_t n=(size_t)(cl-a-1);if(!n||n>=sizeof host)return 0;memcpy(host,a+1,n);host[n]=0;bracketed=1;he=cl+1;}
    else {const char*colon=NULL;for(const char*p=a;p<end;p++)if(*p==':')colon=p;if(colon)he=colon;size_t n=(size_t)(he-a);if(!n||n>=sizeof host)return 0;memcpy(host,a,n);host[n]=0;}
    for(char*p=host;*p;p++)*p=(char)tolower((unsigned char)*p);
    if(bracketed||ip_or_localhost(host))return 0;
    if(!nidna_to_ascii(host,ascii,sizeof ascii))return 0;
    if(!hsts_match(ascii))return 0;
    int explicit80=he<end&&he[0]==':'&&!strncmp(he,":80",3)&&(he+3==end||he[3]=='/'||he[3]=='?'||he[3]=='#');
    const char *suffix = (he < end && he[0] == ':' && explicit80) ? he + 3 : ((he < end && he[0] == ':') ? he : end);
    int n=snprintf(out,cap,"https://%s%s",ascii,suffix);
    return n>0&&(size_t)n<cap;
}
void websec_hsts_remember(const char *host_ascii,int is_https,const char *headers) {
    if(!is_https||!host_ascii||!*host_ascii||ip_or_localhost(host_ascii)||!headers)return;
    char value[512]="";const char*p=headers;
    while(*p){const char*e=strchr(p,'\n');size_t n=e?(size_t)(e-p):strlen(p);if(n>=26&&!strncasecmp(p,"Strict-Transport-Security:",26)){size_t off=26;while(off<n&&isspace((unsigned char)p[off]))off++;size_t vl=n-off;if(vl>=sizeof value)vl=sizeof value-1;memcpy(value,p+off,vl);value[vl]=0;break;}if(!e)break;p=e+1;}
    if(!value[0])return;
    long long maxage=-1;int incl=0;char tmp[512];snprintf(tmp,sizeof tmp,"%s",value);char*save=NULL;
    for(char*t=strtok_r(tmp,";",&save);t;t=strtok_r(NULL,";",&save)){while(isspace((unsigned char)*t))t++;size_t tl=strlen(t);while(tl&&isspace((unsigned char)t[tl-1]))t[--tl]=0;if(!strncasecmp(t,"max-age",7)){char*q=t+7;while(isspace((unsigned char)*q))q++;if(*q=='='){q++;while(isspace((unsigned char)*q))q++;char*end=NULL;long long x=strtoll(q,&end,10);if(end!=q&&!*end)maxage=x;}}else if(!strcasecmp(t,"includeSubDomains"))incl=1;}
    if(maxage<0)return; if(maxage>63072000)maxage=63072000;
    security_dir();
#ifndef _WIN32
    int lk=open("cache/security/hsts.lock",O_CREAT|O_RDWR,0600);if(lk>=0)flock(lk,LOCK_EX);
#endif
    char lines[1024][512];int n=0;FILE*f=fopen("cache/security/hsts.txt","r");if(f){char line[512];while(n<1023&&fgets(line,sizeof line,f)){char h[256];long long exp;int ib;if(sscanf(line,"%255s %lld %d",h,&exp,&ib)==3&&strcasecmp(h,host_ascii)&&exp>(long long)time(NULL)){snprintf(lines[n++],sizeof lines[0],"%s %lld %d\n",h,exp,ib);}}fclose(f);}
    if(maxage>0&&n<1023)snprintf(lines[n++],sizeof lines[0],"%s %lld %d\n",host_ascii,(long long)time(NULL)+maxage,incl);
    char tmpname[128];snprintf(tmpname,sizeof tmpname,"cache/security/hsts.%ld.tmp",(long)getpid());f=fopen(tmpname,"w");if(f){for(int i=0;i<n;i++)fputs(lines[i],f);if(fclose(f)==0)rename(tmpname,"cache/security/hsts.txt");else remove(tmpname);}
#ifndef _WIN32
    if(lk>=0){flock(lk,LOCK_UN);close(lk);}
#endif
}
