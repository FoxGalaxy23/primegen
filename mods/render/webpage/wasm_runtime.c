#include "wasm_runtime.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <math.h>

#define WASM_PAGE_SIZE 65536u
#define WASM_MAX_PAGES 1024u
#define WASM_MAX_FUNCS 4096u
#define WASM_MAX_TYPES 2048u
#define WASM_MAX_EXPORTS 4096u
#define WASM_MAX_LOCALS 4096u
#define WASM_MAX_STACK 2048u
#define WASM_MAX_CALL_DEPTH 128u
#define WASM_MAX_TABLE_ELEMS 65536u
#define WASM_MAX_RESULTS 32u
#define WASM_FUEL 10000000ull

typedef struct Cur { const uint8_t *p; size_t n, pos; int bad; } Cur;
typedef struct WType { uint8_t *params; uint32_t nparams; uint8_t *result_types; uint32_t nresults; uint8_t result; int has_result; } WType;
typedef struct WFunc { uint32_t type; uint8_t *locals; uint32_t nlocals; const uint8_t *code; size_t code_len; uint32_t import_index; int is_import; } WFunc;
typedef struct MlbWasmGlobal { uint8_t type, mut; uint64_t bits; uint32_t refs; int heap_owned; int registered; struct MlbWasmGlobal *registry_next; } WGlobal;
typedef struct WFuncRef { MlbWasmInstance *instance; uint32_t function_index; uint64_t external_ref; } WFuncRef;
typedef struct WRefHandle { WFuncRef ref; uint32_t generation, next_free; int active; } WRefHandle;
typedef struct MlbWasmTable { WFuncRef *entries; uint32_t size, min_size, max_size, refs; uint8_t element_type; int has_max, heap_owned; struct MlbWasmTable *registry_next; } WTable;
typedef struct WExport { char *name; int kind; uint32_t index; } WExport;
typedef struct WImport { char *module_name, *name; int kind; uint32_t type; uint8_t value_type, mut; uint32_t min, max; int has_max; } WImport;
typedef struct WData { uint32_t offset; uint8_t mode; uint8_t *bytes; size_t length; } WData;
/* Element values are function indices; UINT32_MAX represents a typed null. mode:
 * 0 active, 1 passive, 2 declarative. externref element expressions currently support null only. */
typedef struct WElem { uint32_t offset; uint32_t *funcs; size_t count; uint8_t mode, ref_type; } WElem;
typedef struct WCustom { char *name; const uint8_t *bytes; size_t length; } WCustom;
typedef struct MlbWasmMemory { uint8_t *data; uint32_t pages, min_pages, max_pages; int has_max; uint64_t epoch; uint32_t refs; } WMemory;
typedef struct WControl {
    uint8_t kind, result_type;
    int has_result, stack_height;
    const uint8_t *param_types; uint32_t nparams;
    const uint8_t *result_types; uint32_t nresults;
    size_t body_pos, else_pos, end_pos;
} WControl;
typedef struct WBlockType { int indexed, has_result; uint32_t type_index; uint8_t result_type; } WBlockType;

struct MlbWasmModule {
    uint8_t *binary; size_t binary_len;
    WType *types; uint32_t ntypes;
    WFunc *funcs; uint32_t nfuncs, defined_funcs, nimport_funcs;
    uint32_t *func_types;
    WImport *imports; uint32_t nimports;
    WGlobal *globals; uint32_t nglobals;
    uint32_t min_pages, max_pages; int has_memory, has_memory_max, has_memory_import; uint32_t memory_import_index;
    uint32_t table_min, table_max; uint8_t table_type; int has_table, has_table_max, has_table_import; uint32_t table_import_index;
    uint32_t nimport_globals;
    WElem *elements; uint32_t nelements;
    WExport *exports; uint32_t nexports;
    WData *data; uint32_t ndata;
    WCustom *custom; uint32_t ncustom;
    int start_seen; uint32_t start_func;
    int has_data_count, needs_data_count; uint32_t data_count;
};
struct MlbWasmInstance { MlbWasmModule *module; WGlobal **globals; uint32_t nglobals; WMemory *memory; int has_memory; WTable *table; int has_table; WFuncRef *function_aliases; uint8_t *data_dropped; uint8_t *elem_dropped; MlbWasmHostCall host_call; MlbWasmHostCallMulti host_call_multi; void *host_opaque; char error[192]; };
static WTable *g_wasm_tables;
static WGlobal *g_wasm_globals;
/* Stable numeric handles let reference values travel through the interpreter's
 * uint64_t operand/local slots without confusing function indices from
 * different instances. Handle 0 is the null reference. */
static WRefHandle *g_wasm_ref_handles;
static size_t g_wasm_ref_handle_count, g_wasm_ref_handle_capacity;
static uint32_t g_wasm_ref_free_head; /* slot index + 1, zero means empty */

static WFuncRef wasm_function_ref_canonical(WFuncRef ref) {
    /* Imported WASM functions are aliases of the function supplied by the
     * importing module. Follow the binding map so ref.func/table.get use the
     * same identity on both sides of an import boundary. The setter rejects
     * cycles; the depth bound is a defensive guard for corrupted state. */
    for (unsigned depth = 0; depth < WASM_MAX_CALL_DEPTH; ++depth) {
        MlbWasmInstance *owner = ref.instance;
        if (!owner || !owner->module || !owner->function_aliases ||
            ref.function_index >= owner->module->nimport_funcs) break;
        WFuncRef next = owner->function_aliases[ref.function_index];
        if (!next.instance || !next.instance->module ||
            next.function_index >= next.instance->module->nfuncs) break;
        if (next.instance == owner && next.function_index == ref.function_index) break;
        ref = next;
    }
    return ref;
}

static uint64_t wasm_ref_token_for(WFuncRef ref);
static int wasm_ref_from_token(uint64_t token, WFuncRef *out);
static int wasm_ref_value_normalize(MlbWasmValue *value) {
    if (!value || value->type != MLB_WASM_FUNCREF || value->bits == 0) return value != NULL;
    WFuncRef ref;
    if (!wasm_ref_from_token(value->bits, &ref)) return 0;
    uint64_t token = wasm_ref_token_for(ref);
    if (!token) return 0;
    value->bits = token;
    return 1;
}

static int errf(char *e,size_t c,const char *s){if(e&&c)snprintf(e,c,"%s",s);return 0;}
static uint8_t rd8(Cur*c){if(c->pos>=c->n){c->bad=1;return 0;}return c->p[c->pos++];}
static int rd_bytes(Cur*c,size_t n,const uint8_t **p){if(n>c->n-c->pos){c->bad=1;return 0;}*p=c->p+c->pos;c->pos+=n;return 1;}
static int rd_u32(Cur*c,uint32_t*out){uint32_t v=0;unsigned sh=0;for(int i=0;i<5;i++){uint8_t b=rd8(c);if(c->bad)return 0;if(i==4&&(b&0xf0))return c->bad=1,0;v|=(uint32_t)(b&127)<<sh;if(!(b&128)){*out=v;return 1;}sh+=7;}c->bad=1;return 0;}
/* WebAssembly signed LEB128 is allowed to be padded, but its final byte must
 * still sign-extend the declared integer width. Without this check, e.g. an
 * out-of-range 5-byte s32 immediate could be silently truncated to a valid
 * value by the native interpreter. */
static int64_t rd_sleb(Cur*c,unsigned bits){
    uint64_t v=0; unsigned sh=0; uint8_t b=0; unsigned max=(bits+6)/7;
    if(bits!=32&&bits!=64){c->bad=1;return 0;}
    for(unsigned i=0;i<max;i++){
        b=rd8(c); if(c->bad)return 0;
        uint8_t payload=(uint8_t)(b&0x7f);
        if(i==max-1){
            unsigned used=bits-7*i;
            if(used<7){
                uint8_t low_mask=(uint8_t)((1u<<used)-1u);
                uint8_t high_mask=(uint8_t)(0x7fu&~low_mask);
                int sign=(payload>>(used-1))&1;
                if((!sign&&(payload&high_mask)!=0)||(sign&&(payload&high_mask)!=high_mask)){
                    c->bad=1;return 0;
                }
            }
        }
        v|=(uint64_t)payload<<sh; sh+=7;
        if(!(b&0x80u)){
            if(sh<bits&&(b&0x40u))v|=(~(uint64_t)0)<<sh;
            if(bits<64){uint64_t mask=((uint64_t)1<<bits)-1;v&=mask;if(v&((uint64_t)1<<(bits-1)))v|=~mask;}
            return (int64_t)v;
        }
    }
    c->bad=1;return 0;
}
static int valid_utf8(const uint8_t*p,size_t n){
    size_t i=0;
    while(i<n){
        uint8_t a=p[i++];
        if(a<0x80)continue;
        if(a>=0xc2&&a<=0xdf){
            if(i>=n||(p[i]&0xc0)!=0x80)return 0;
            i++;continue;
        }
        if(a>=0xe0&&a<=0xef){
            if(i+1>=n)return 0;
            uint8_t b=p[i],d=p[i+1];
            if((d&0xc0)!=0x80||(b&0xc0)!=0x80)return 0;
            if((a==0xe0&&b<0xa0)||(a==0xed&&b>=0xa0))return 0;
            i+=2;continue;
        }
        if(a>=0xf0&&a<=0xf4){
            if(i+2>=n)return 0;
            uint8_t b=p[i],d=p[i+1],e=p[i+2];
            if((b&0xc0)!=0x80||(d&0xc0)!=0x80||(e&0xc0)!=0x80)return 0;
            if((a==0xf0&&b<0x90)||(a==0xf4&&b>=0x90))return 0;
            i+=3;continue;
        }
        return 0;
    }
    return 1;
}
static int rd_name(Cur*c,char**out){
    uint32_t n; if(!rd_u32(c,&n)||n>c->n-c->pos)return 0;
    if(!valid_utf8(c->p+c->pos,n))return 0;
    char*s=malloc((size_t)n+1);if(!s)return 0;
    memcpy(s,c->p+c->pos,n);s[n]=0;c->pos+=n;*out=s;return 1;
}
static int valid_ref_type(uint8_t t){return t==MLB_WASM_EXTERNREF||t==MLB_WASM_FUNCREF;}
static int valid_valtype(uint8_t t){return t==MLB_WASM_I32||t==MLB_WASM_I64||t==MLB_WASM_F32||t==MLB_WASM_F64||valid_ref_type(t);}
static int valid_localtype(uint8_t t){return valid_valtype(t);}
static int parse_limits(Cur*c,uint32_t*min,uint32_t*max,int*hasmax){uint32_t flags; if(!rd_u32(c,&flags)||flags>1)return 0;if(!rd_u32(c,min))return 0;*hasmax=(int)(flags&1);*max=*min;if(*hasmax&&!rd_u32(c,max))return 0;if(*min>WASM_MAX_PAGES||*max>WASM_MAX_PAGES||(*hasmax&&*max<*min))return 0;return 1;}
static int parse_const_expr(Cur*c,uint8_t*type,uint64_t*bits){uint8_t op=rd8(c);if(c->bad)return 0;if(op==0x41){*type=MLB_WASM_I32;*bits=(uint32_t)rd_sleb(c,32);}else if(op==0x42){*type=MLB_WASM_I64;*bits=(uint64_t)rd_sleb(c,64);}else if(op==0x43){const uint8_t*p;if(!rd_bytes(c,4,&p))return 0;uint32_t v=(uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;*type=MLB_WASM_F32;*bits=v;}else if(op==0x44){const uint8_t*p;if(!rd_bytes(c,8,&p))return 0;uint64_t v=0;for(int i=7;i>=0;i--)v=(v<<8)|p[i];*type=MLB_WASM_F64;*bits=v;}else if(op==0xd0){uint8_t rt=rd8(c);if(c->bad||!valid_ref_type(rt))return 0;*type=rt;*bits=0;}else return 0;return rd8(c)==0x0b&&!c->bad;}
/* Parse one MVP/reference-types element expression. The compact runtime stores
 * function references as function indices and UINT32_MAX for ref.null. */
static int parse_elem_expr(Cur *c, uint32_t *function_index, uint8_t *ref_type) {
    uint8_t op = rd8(c);
    if (c->bad) return 0;
    if (op == 0xd2) {
        if (!rd_u32(c, function_index)) return 0;
        if (ref_type) *ref_type = MLB_WASM_FUNCREF;
    } else if (op == 0xd0) {
        uint8_t rt = rd8(c);
        if (c->bad || !valid_ref_type(rt)) return 0;
        *function_index = UINT32_MAX;
        if (ref_type) *ref_type = rt;
    } else {
        return 0;
    }
    return rd8(c) == 0x0b && !c->bad;
}
static int skip_memarg(Cur*c){uint32_t a,o;return rd_u32(c,&a)&&rd_u32(c,&o);}
static int64_t rd_sleb_from_first(Cur *c, unsigned bits, uint8_t first) {
    uint64_t v = 0; unsigned sh = 0; uint8_t b = first;
    unsigned max = (bits + 6) / 7;
    if (bits < 2 || bits > 64) { c->bad = 1; return 0; }
    for (unsigned i = 0; i < max; ++i) {
        if (i) b = rd8(c);
        if (c->bad) return 0;
        uint8_t payload = (uint8_t)(b & 0x7f);
        if (i == max - 1) {
            unsigned used = bits - 7 * i;
            if (used < 7) {
                uint8_t low_mask = (uint8_t)((1u << used) - 1u);
                uint8_t high_mask = (uint8_t)(0x7fu & ~low_mask);
                int sign = (payload >> (used - 1)) & 1;
                if ((!sign && (payload & high_mask)) || (sign && (payload & high_mask) != high_mask)) {
                    c->bad = 1; return 0;
                }
            }
        }
        v |= (uint64_t)payload << sh; sh += 7;
        if (!(b & 0x80u)) {
            if (bits < 64) {
                uint64_t mask = ((uint64_t)1 << bits) - 1u;
                v &= mask;
                if (v & ((uint64_t)1 << (bits - 1))) v |= ~mask;
            }
            return (int64_t)v;
        }
    }
    c->bad = 1; return 0;
}
/* blocktype is either empty, a single value type, or a signed-LEB33 type index. */
static int read_block_type(Cur *c, WBlockType *out) {
    uint8_t b = rd8(c);
    if (c->bad || !out) return 0;
    memset(out, 0, sizeof(*out));
    if (b == 0x40) return 1;
    if (valid_valtype(b)) { out->has_result = 1; out->result_type = b; return 1; }
    int64_t idx;
    if (!(b & 0x80u)) {
        if (b > 0x3f) return 0;
        idx = b;
    } else {
        idx = rd_sleb_from_first(c, 33, b);
        if (c->bad) return 0;
    }
    if (idx < 0 || (uint64_t)idx > UINT32_MAX) return 0;
    out->indexed = 1; out->type_index = (uint32_t)idx;
    return 1;
}
static int resolve_block_type(const MlbWasmModule *m, const WBlockType *bt,
                              const uint8_t **params, uint32_t *nparams,
                              const uint8_t **results, uint32_t *nresults) {
    if (params) *params = NULL;
    if (nparams) *nparams = 0;
    if (results) *results = NULL;
    if (nresults) *nresults = 0;
    if (!bt) return 0;
    if (!bt->indexed) {
        /* Single-result block types keep their type inline in WBlockType; do
         * not return a pointer into that temporary stack object. */
        if (nresults) *nresults = bt->has_result ? 1u : 0u;
        return 1;
    }
    if (!m || bt->type_index >= m->ntypes) return 0;
    const WType *t = &m->types[bt->type_index];
    if (params) *params = t->params;
    if (nparams) *nparams = t->nparams;
    if (results) *results = t->result_types;
    if (nresults) *nresults = t->nresults;
    return 1;
}
static int skip_instruction_immediate(Cur*c,uint8_t op){switch(op){case 0x0c:case 0x0d:case 0x10:case 0x20:case 0x21:case 0x22:case 0x23:case 0x24:case 0x25:case 0x26:{uint32_t x;return rd_u32(c,&x);}case 0x0e:{uint32_t n,x;if(!rd_u32(c,&n)||n>WASM_MAX_STACK)return 0;for(uint32_t i=0;i<=n;i++)if(!rd_u32(c,&x))return 0;return 1;}case 0x11:{uint32_t t,tab;return rd_u32(c,&t)&&rd_u32(c,&tab);}case 0x28:case 0x29:case 0x2a:case 0x2b:case 0x2c:case 0x2d:case 0x2e:case 0x2f:case 0x30:case 0x31:case 0x32:case 0x33:case 0x34:case 0x35:case 0x36:case 0x37:case 0x38:case 0x39:case 0x3a:case 0x3b:case 0x3c:case 0x3d:case 0x3e:return skip_memarg(c);case 0x3f:case 0x40:{uint32_t z;return rd_u32(c,&z)&&z==0;}case 0x41:(void)rd_sleb(c,32);return !c->bad;case 0x42:(void)rd_sleb(c,64);return !c->bad;case 0x43:{const uint8_t*q;return rd_bytes(c,4,&q);}case 0x44:{const uint8_t*q;return rd_bytes(c,8,&q);}case 0xfc:{uint32_t sub,a,b;if(!rd_u32(c,&sub))return 0;switch(sub){case 8:return rd_u32(c,&a)&&rd_u32(c,&b);case 9:case 13:case 15:case 16:case 17:return rd_u32(c,&a);case 10:case 12:case 14:return rd_u32(c,&a)&&rd_u32(c,&b);case 11:return rd_u32(c,&a);default:return 0;}}case 0xd0:{uint8_t t=rd8(c);return !c->bad&&valid_ref_type(t);}case 0xd2:{uint32_t f;return rd_u32(c,&f);}case 0x00:case 0x01:case 0xd1:case 0x05:case 0x0b:case 0x0f:case 0x1a:case 0x1b:case 0x45:case 0x46:case 0x47:case 0x48:case 0x49:case 0x4a:case 0x4b:case 0x4c:case 0x4d:case 0x4e:case 0x4f:case 0x50:case 0x51:case 0x52:case 0x53:case 0x54:case 0x55:case 0x56:case 0x57:case 0x58:case 0x59:case 0x5a:case 0x5b:case 0x5c:case 0x5d:case 0x5e:case 0x5f:case 0x60:case 0x61:case 0x62:case 0x63:case 0x64:case 0x65:case 0x66:case 0x67:case 0x68:case 0x69:case 0x6a:case 0x6b:case 0x6c:case 0x6d:case 0x6e:case 0x6f:case 0x70:case 0x71:case 0x72:case 0x73:case 0x74:case 0x75:case 0x76:case 0x77:case 0x78:case 0x79:case 0x7a:case 0x7b:case 0x7c:case 0x7d:case 0x7e:case 0x7f:case 0x80:case 0x81:case 0x82:case 0x83:case 0x84:case 0x85:case 0x86:case 0x87:case 0x88:case 0x89:case 0x8a:case 0x8b:case 0x8c:case 0x8d:case 0x8e:case 0x8f:case 0x90:case 0x91:case 0x92:case 0x93:case 0x94:case 0x95:case 0x96:case 0x97:case 0x98:case 0x99:case 0x9a:case 0x9b:case 0x9c:case 0x9d:case 0x9e:case 0x9f:case 0xa0:case 0xa1:case 0xa2:case 0xa3:case 0xa4:case 0xa5:case 0xa6:case 0xa7:case 0xa8:case 0xa9:case 0xaa:case 0xab:case 0xac:case 0xad:case 0xae:case 0xaf:case 0xb0:case 0xb1:case 0xb2:case 0xb3:case 0xb4:case 0xb5:case 0xb6:case 0xb7:case 0xb8:case 0xb9:case 0xba:case 0xbb:case 0xbc:case 0xbd:case 0xbe:case 0xbf:case 0xc0:case 0xc1:case 0xc2:case 0xc3:case 0xc4:return 1;default:return 0;}}
static int find_control_bounds(const uint8_t*code,size_t n,size_t body_pos,uint8_t root_kind,size_t*else_pos,size_t*end_pos){Cur c={code,n,body_pos,0};unsigned depth=1;int saw_else=0;if(else_pos)*else_pos=SIZE_MAX;while(c.pos<c.n&&!c.bad){size_t at=c.pos;uint8_t op=rd8(&c);if(c.bad)return 0;if(op==0x02||op==0x03||op==0x04){WBlockType bt;if(!read_block_type(&c,&bt))return 0;depth++;continue;}if(op==0x05){if(depth==1){if(root_kind!=0x04||saw_else)return 0;saw_else=1;if(else_pos)*else_pos=at;}continue;}if(op==0x0b){if(--depth==0){if(end_pos)*end_pos=at;return 1;}continue;}if(!skip_instruction_immediate(&c,op))return 0;}return 0;}
static int scan_code(const uint8_t*p,size_t n,int*needs_data_count){Cur c={p,n,0,0};uint8_t kinds[WASM_MAX_CALL_DEPTH];uint8_t else_seen[WASM_MAX_CALL_DEPTH];unsigned depth=0;while(c.pos<c.n){size_t at=c.pos;uint8_t op=rd8(&c);if(c.bad)return 0;if(op==0xfc&&needs_data_count){Cur q={p,n,c.pos,0};uint32_t sub;if(!rd_u32(&q,&sub))return 0;if(sub==8||sub==9)*needs_data_count=1;}if(op==0x02||op==0x03||op==0x04){WBlockType bt;size_t ep,en;if(depth>=WASM_MAX_CALL_DEPTH||!read_block_type(&c,&bt)||!find_control_bounds(p,n,c.pos,op,&ep,&en))return 0;kinds[depth]=op;else_seen[depth]=0;depth++;continue;}if(op==0x05){if(!depth||kinds[depth-1]!=0x04||else_seen[depth-1])return 0;else_seen[depth-1]=1;continue;}if(op==0x0b){if(depth){depth--;continue;}return c.pos==c.n;}if(!skip_instruction_immediate(&c,op))return 0;if(c.bad)return 0;(void)at;}return 0;}
typedef struct VCtrl {
    uint8_t kind, result_type;
    int has_result;
    size_t height;
    int unreachable, else_seen;
    const uint8_t *param_types; uint32_t nparams;
    const uint8_t *result_types; uint32_t nresults;
} VCtrl;
#define WASM_VALIDATE_STACK 8192u
#define WASM_VALIDATE_CTRL 256u
static int vpush(uint8_t *st, size_t *sp, uint8_t t) {
    if (*sp >= WASM_VALIDATE_STACK) return 0;
    st[(*sp)++] = t;
    return 1;
}
static int vpop(uint8_t *st, size_t *sp, VCtrl *cur, uint8_t want, uint8_t *got) {
    uint8_t t;
    if (*sp == cur->height && cur->unreachable) {
        if (got) *got = 0;
        return 1;
    }
    if (*sp <= cur->height) return 0;
    t = st[--(*sp)];
    if (want && t && t != want) return 0;
    if (got) *got = t;
    return 1;
}
static void vmark_unreachable(uint8_t *st, size_t *sp, VCtrl *cur) {
    (void)st;
    *sp = cur->height;
    cur->unreachable = 1;
}
static uint32_t vctrl_result_count(const VCtrl *cur) {
    return cur->result_types ? cur->nresults : (cur->has_result ? 1u : 0u);
}
static uint8_t vctrl_result_type(const VCtrl *cur, uint32_t index) {
    return cur->result_types ? cur->result_types[index] : cur->result_type;
}
static uint32_t vctrl_label_count(const VCtrl *cur) {
    return cur->kind == 0x03 ? cur->nparams : vctrl_result_count(cur);
}
static uint8_t vctrl_label_type(const VCtrl *cur, uint32_t index) {
    return cur->kind == 0x03 ? cur->param_types[index] : vctrl_result_type(cur, index);
}
static int vpop_ctrl_labels(uint8_t *st, size_t *sp, VCtrl *cur, const VCtrl *target,
                            uint8_t *out_types, uint32_t out_capacity) {
    uint32_t count = vctrl_label_count(target);
    if (count > out_capacity) return 0;
    for (uint32_t k = count; k > 0; --k) {
        uint8_t got = 0, want = vctrl_label_type(target, k - 1);
        if (!vpop(st, sp, cur, want, &got)) return 0;
        if (out_types) out_types[k - 1] = got ? got : want;
    }
    return 1;
}
static int vpop_ctrl_results(uint8_t *st, size_t *sp, VCtrl *cur, const VCtrl *target,
                             uint8_t *out_types, uint32_t out_capacity) {
    uint32_t count = vctrl_result_count(target);
    if (count > out_capacity) return 0;
    for (uint32_t k = count; k > 0; --k) {
        uint8_t got = 0, want = vctrl_result_type(target, k - 1);
        if (!vpop(st, sp, cur, want, &got)) return 0;
        if (out_types) out_types[k - 1] = got ? got : want;
    }
    return 1;
}
static int vfinish_ctrl(uint8_t *st, size_t *sp, VCtrl *cur) {
    if (!vpop_ctrl_results(st, sp, cur, cur, NULL, WASM_MAX_RESULTS)) return 0;
    return *sp == cur->height;
}
static uint8_t v_load_type(uint8_t op) {
    if (op == 0x28 || (op >= 0x2c && op <= 0x2f)) return MLB_WASM_I32;
    if (op == 0x29 || (op >= 0x30 && op <= 0x35)) return MLB_WASM_I64;
    if (op == 0x2a) return MLB_WASM_F32;
    if (op == 0x2b) return MLB_WASM_F64;
    return 0;
}
static uint8_t v_store_type(uint8_t op) {
    if (op == 0x36 || op == 0x3a || op == 0x3b) return MLB_WASM_I32;
    if (op == 0x37 || (op >= 0x3c && op <= 0x3e)) return MLB_WASM_I64;
    if (op == 0x38) return MLB_WASM_F32;
    if (op == 0x39) return MLB_WASM_F64;
    return 0;
}
static int vmemarg(Cur *c, uint8_t op) {
    uint32_t align, offset;
    unsigned max_align = 0;
    (void)offset;
    if (!rd_u32(c, &align) || !rd_u32(c, &offset)) return 0;
    switch (op) {
        case 0x28: case 0x2a: case 0x34: case 0x35: case 0x36: case 0x38: case 0x3e: max_align = 2; break;
        case 0x29: case 0x2b: case 0x37: case 0x39: max_align = 3; break;
        case 0x2e: case 0x2f: case 0x32: case 0x33: case 0x3b: case 0x3d: max_align = 1; break;
        default: max_align = 0; break;
    }
    return align <= max_align;
}
static int module_declares_function_ref(const MlbWasmModule *m, uint32_t function_index) {
    if (!m) return 0;
    for (uint32_t e = 0; e < m->nelements; ++e)
        for (size_t k = 0; k < m->elements[e].count; ++k)
            if (m->elements[e].funcs[k] == function_index) return 1;
    return 0;
}
static int validate_func_types(const MlbWasmModule *m, const WFunc *f) {
    const WType *ft = &m->types[f->type];
    size_t cap = WASM_VALIDATE_STACK, sp = 0;
    uint8_t *st = malloc(cap);
    VCtrl controls[WASM_VALIDATE_CTRL];
    unsigned cd = 1;
    Cur c = { f->code, f->code_len, 0, 0 };
    int ok = 0, ended = 0;
    uint32_t nlocals = ft->nparams + f->nlocals;
    if (!st) return 0;
    controls[0] = (VCtrl){0};
    controls[0].kind = 0xff; controls[0].result_type = ft->result;
    controls[0].has_result = ft->has_result; controls[0].height = 0;
    controls[0].result_types = ft->result_types; controls[0].nresults = ft->nresults;
    while (c.pos < c.n && !c.bad && !ended) {
        uint8_t op = rd8(&c), a_type = 0, b_type = 0, out_type = 0;
        uint32_t idx = 0, idx2 = 0;
        VCtrl *cur = &controls[cd - 1];
        switch (op) {
            case 0x00:
                vmark_unreachable(st, &sp, cur);
                break;
            case 0x01:
                break;
            case 0x02: case 0x03: case 0x04: {
                WBlockType bt; const uint8_t *params = NULL, *results = NULL;
                uint32_t nparams = 0, nresults = 0;
                if (cd >= WASM_VALIDATE_CTRL || (op == 0x04 && !vpop(st, &sp, cur, MLB_WASM_I32, NULL)) ||
                    !read_block_type(&c, &bt) || !resolve_block_type(m, &bt, &params, &nparams, &results, &nresults) ||
                    nresults > WASM_MAX_RESULTS || nparams > WASM_MAX_RESULTS) goto done;
                for (uint32_t j = nparams; j > 0; --j)
                    if (!vpop(st, &sp, cur, params[j - 1], NULL)) goto done;
                VCtrl next = {0};
                next.kind = op; next.result_type = bt.result_type; next.has_result = bt.has_result;
                /* Each structured control frame starts reachable for validation.
                 * An unreachable parent makes operand pops polymorphic only at its own
                 * frame height; it must not waive the child block's result checks. */
                next.height = sp; next.unreachable = 0;
                next.param_types = params; next.nparams = nparams;
                next.result_types = bt.indexed ? results : NULL; next.nresults = bt.indexed ? nresults : 0;
                controls[cd++] = next;
                for (uint32_t j = 0; j < nparams; ++j) if (!vpush(st, &sp, params[j])) goto done;
                break;
            }
            case 0x05: {
                if (cd <= 1 || cur->kind != 0x04 || cur->else_seen || !vfinish_ctrl(st, &sp, cur)) goto done;
                sp = cur->height;
                for (uint32_t j = 0; j < cur->nparams; ++j) if (!vpush(st, &sp, cur->param_types[j])) goto done;
                cur->else_seen = 1;
                /* The else arm is validated as its own reachable arm. */
                cur->unreachable = 0;
                break;
            }
            case 0x0b: {
                if (cd == 1) {
                    if (!vfinish_ctrl(st, &sp, cur) || c.pos != c.n) goto done;
                    ended = 1;
                    break;
                }
                if (cur->kind == 0x04 && !cur->else_seen) {
                    uint32_t nr = vctrl_result_count(cur);
                    if (nr != cur->nparams) goto done;
                    for (uint32_t j = 0; j < nr; ++j)
                        if (vctrl_result_type(cur, j) != cur->param_types[j]) goto done;
                }
                if (!vfinish_ctrl(st, &sp, cur)) goto done;
                {
                    uint32_t nr = vctrl_result_count(cur);
                    uint8_t rt[WASM_MAX_RESULTS];
                    for (uint32_t j = nr; j > 0; --j) rt[j - 1] = vctrl_result_type(cur, j - 1);
                    sp = cur->height; cd--;
                    for (uint32_t j = 0; j < nr; ++j) if (!vpush(st, &sp, rt[j])) goto done;
                }
                break;
            }
            case 0x0c: case 0x0d: {
                int conditional = op == 0x0d;
                if (!rd_u32(&c, &idx) || idx >= cd) goto done;
                if (conditional && !vpop(st, &sp, cur, MLB_WASM_I32, NULL)) goto done;
                VCtrl *target = &controls[cd - 1 - idx];
                uint32_t label_count = vctrl_label_count(target);
                uint8_t branch_types[WASM_MAX_RESULTS];
                if (label_count > WASM_MAX_RESULTS || !vpop_ctrl_labels(st, &sp, cur, target, branch_types, WASM_MAX_RESULTS)) goto done;
                if (conditional) for (uint32_t k = 0; k < label_count; ++k)
                    if (!vpush(st, &sp, branch_types[k])) goto done;
                if (!conditional) vmark_unreachable(st, &sp, cur);
                break;
            }
            case 0x0e: {
                uint32_t count, label; VCtrl *first = NULL;
                if (!rd_u32(&c, &count) || count > WASM_VALIDATE_STACK) goto done;
                for (uint32_t j = 0; j <= count; ++j) {
                    if (!rd_u32(&c, &label) || label >= cd) goto done;
                    VCtrl *target = &controls[cd - 1 - label];
                    if (!first) first = target;
                    else {
                        uint32_t nc = vctrl_label_count(first), nt = vctrl_label_count(target);
                        if (nc != nt) goto done;
                        for (uint32_t k = 0; k < nc; ++k)
                            if (vctrl_label_type(first, k) != vctrl_label_type(target, k)) goto done;
                    }
                }
                if (!first || !vpop(st, &sp, cur, MLB_WASM_I32, NULL)) goto done;
                uint8_t branch_types[WASM_MAX_RESULTS];
                if (vctrl_label_count(first) > WASM_MAX_RESULTS ||
                    !vpop_ctrl_labels(st, &sp, cur, first, branch_types, WASM_MAX_RESULTS)) goto done;
                vmark_unreachable(st, &sp, cur);
                break;
            }
            case 0x0f:
                for (uint32_t j = ft->nresults; j > 0; --j)
                    if (!vpop(st, &sp, cur, ft->result_types[j - 1], NULL)) goto done;
                vmark_unreachable(st, &sp, cur);
                break;
            case 0x10: {
                if (!rd_u32(&c, &idx) || idx >= m->nfuncs) goto done;
                const WType *ct = &m->types[m->funcs[idx].type];
                for (uint32_t j = ct->nparams; j > 0; --j)
                    if (!vpop(st, &sp, cur, ct->params[j - 1], NULL)) goto done;
                for (uint32_t j = 0; j < ct->nresults; ++j) if (!vpush(st, &sp, ct->result_types[j])) goto done;
                break;
            }
            case 0x11: {
                if (!rd_u32(&c, &idx) || !rd_u32(&c, &idx2) || idx >= m->ntypes || idx2 != 0 || !m->has_table || !vpop(st, &sp, cur, MLB_WASM_I32, NULL)) goto done;
                const WType *ct = &m->types[idx];
                for (uint32_t j = ct->nparams; j > 0; --j)
                    if (!vpop(st, &sp, cur, ct->params[j - 1], NULL)) goto done;
                for (uint32_t j = 0; j < ct->nresults; ++j) if (!vpush(st, &sp, ct->result_types[j])) goto done;
                break;
            }
            case 0x1a:
                if (!vpop(st, &sp, cur, 0, NULL)) goto done;
                break;
            case 0x1b:
                if (!vpop(st, &sp, cur, MLB_WASM_I32, NULL) || !vpop(st, &sp, cur, 0, &b_type) || !vpop(st, &sp, cur, 0, &a_type)) goto done;
                if (a_type && b_type && a_type != b_type) goto done;
                if (!vpush(st, &sp, a_type ? a_type : b_type)) goto done;
                break;
            case 0x20: case 0x21: case 0x22:
                if (!rd_u32(&c, &idx) || idx >= nlocals) goto done;
                out_type = idx < ft->nparams ? ft->params[idx] : f->locals[idx - ft->nparams];
                if (op == 0x20) { if (!vpush(st, &sp, out_type)) goto done; }
                else if (op == 0x21) { if (!vpop(st, &sp, cur, out_type, NULL)) goto done; }
                else { if (!vpop(st, &sp, cur, out_type, NULL) || !vpush(st, &sp, out_type)) goto done; }
                break;
            case 0x23: case 0x24:
                if (!rd_u32(&c, &idx) || idx >= m->nglobals) goto done;
                out_type = m->globals[idx].type;
                if (op == 0x23) { if (!vpush(st, &sp, out_type)) goto done; }
                else if (!m->globals[idx].mut || !vpop(st, &sp, cur, out_type, NULL)) goto done;
                break;
            case 0x25: /* table.get */
                if (!rd_u32(&c, &idx) || idx != 0 || !m->has_table ||
                    !vpop(st, &sp, cur, MLB_WASM_I32, NULL) ||
                    !vpush(st, &sp, m->table_type)) goto done;
                break;
            case 0x26: /* table.set */
                if (!rd_u32(&c, &idx) || idx != 0 || !m->has_table ||
                    !vpop(st, &sp, cur, m->table_type, NULL) ||
                    !vpop(st, &sp, cur, MLB_WASM_I32, NULL)) goto done;
                break;
            case 0x28: case 0x29: case 0x2a: case 0x2b: case 0x2c: case 0x2d: case 0x2e: case 0x2f: case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35:
                if (!m->has_memory || !vpop(st, &sp, cur, MLB_WASM_I32, NULL) || !vmemarg(&c, op) || !(out_type = v_load_type(op)) || !vpush(st, &sp, out_type)) goto done;
                break;
            case 0x36: case 0x37: case 0x38: case 0x39: case 0x3a: case 0x3b: case 0x3c: case 0x3d: case 0x3e:
                if (!m->has_memory || !vpop(st, &sp, cur, v_store_type(op), NULL) || !vpop(st, &sp, cur, MLB_WASM_I32, NULL) || !vmemarg(&c, op)) goto done;
                break;
            case 0x3f:
                if (!rd_u32(&c, &idx) || idx != 0 || !m->has_memory || !vpush(st, &sp, MLB_WASM_I32)) goto done;
                break;
            case 0x40:
                if (!rd_u32(&c, &idx) || idx != 0 || !m->has_memory || !vpop(st, &sp, cur, MLB_WASM_I32, NULL) || !vpush(st, &sp, MLB_WASM_I32)) goto done;
                break;
            case 0xfc: {
                uint32_t subop, dataidx, memidx;
                if (!rd_u32(&c, &subop)) goto done;
                switch (subop) {
                    case 8: /* memory.init dataidx memidx */
                        if (!rd_u32(&c, &dataidx) || !rd_u32(&c, &memidx) ||
                            dataidx >= m->ndata || memidx != 0 || !m->has_memory ||
                            !vpop(st, &sp, cur, MLB_WASM_I32, NULL) ||
                            !vpop(st, &sp, cur, MLB_WASM_I32, NULL) ||
                            !vpop(st, &sp, cur, MLB_WASM_I32, NULL)) goto done;
                        break;
                    case 9: /* data.drop dataidx */
                        if (!rd_u32(&c, &dataidx) || dataidx >= m->ndata) goto done;
                        break;
                    case 10: /* memory.copy dstmem srcmem */
                        if (!rd_u32(&c, &memidx) || memidx != 0 ||
                            !rd_u32(&c, &memidx) || memidx != 0 || !m->has_memory ||
                            !vpop(st, &sp, cur, MLB_WASM_I32, NULL) ||
                            !vpop(st, &sp, cur, MLB_WASM_I32, NULL) ||
                            !vpop(st, &sp, cur, MLB_WASM_I32, NULL)) goto done;
                        break;
                    case 11: /* memory.fill memidx */
                        if (!rd_u32(&c, &memidx) || memidx != 0 || !m->has_memory ||
                            !vpop(st, &sp, cur, MLB_WASM_I32, NULL) ||
                            !vpop(st, &sp, cur, MLB_WASM_I32, NULL) ||
                            !vpop(st, &sp, cur, MLB_WASM_I32, NULL)) goto done;
                        break;
                    case 12: { /* table.init elemidx tableidx */
                        uint32_t elemidx, tableidx;
                        if(!rd_u32(&c,&elemidx)||!rd_u32(&c,&tableidx)||elemidx>=m->nelements||m->elements[elemidx].mode!=1||tableidx!=0||!m->has_table||m->elements[elemidx].ref_type!=m->table_type||
                           !vpop(st,&sp,cur,MLB_WASM_I32,NULL)||!vpop(st,&sp,cur,MLB_WASM_I32,NULL)||!vpop(st,&sp,cur,MLB_WASM_I32,NULL))goto done;
                        break;}
                    case 13: if(!rd_u32(&c,&dataidx)||dataidx>=m->nelements)goto done; break; /* elem.drop */
                    case 14: {uint32_t dsttab,srctab;if(!rd_u32(&c,&dsttab)||!rd_u32(&c,&srctab)||dsttab!=0||srctab!=0||!m->has_table||!vpop(st,&sp,cur,MLB_WASM_I32,NULL)||!vpop(st,&sp,cur,MLB_WASM_I32,NULL)||!vpop(st,&sp,cur,MLB_WASM_I32,NULL))goto done;break;} /* table.copy */
                    case 15: if(!rd_u32(&c,&memidx)||memidx!=0||!m->has_table||!vpop(st,&sp,cur,MLB_WASM_I32,NULL)||!vpop(st,&sp,cur,m->table_type,NULL))goto done;if(!vpush(st,&sp,MLB_WASM_I32))goto done;break; /* table.grow */
                    case 16: if(!rd_u32(&c,&memidx)||memidx!=0||!m->has_table||!vpush(st,&sp,MLB_WASM_I32))goto done;break; /* table.size */
                    case 17: if(!rd_u32(&c,&memidx)||memidx!=0||!m->has_table||!vpop(st,&sp,cur,MLB_WASM_I32,NULL)||!vpop(st,&sp,cur,m->table_type,NULL)||!vpop(st,&sp,cur,MLB_WASM_I32,NULL))goto done;break; /* table.fill */
                    default:
                        goto done;
                }
                break;
            }
            case 0xd0: { uint8_t rt=rd8(&c); if(c.bad||!valid_ref_type(rt)||!vpush(st,&sp,rt))goto done; break; } /* ref.null */
            case 0xd1: if(!vpop(st,&sp,cur,0,&a_type)||(a_type&&!valid_ref_type(a_type))||!vpush(st,&sp,MLB_WASM_I32))goto done; break; /* ref.is_null */
            case 0xd2: if(!rd_u32(&c,&idx)||idx>=m->nfuncs||!module_declares_function_ref(m,idx)||!vpush(st,&sp,0x70))goto done; break; /* ref.func */
            case 0x41: (void)rd_sleb(&c, 32); if (c.bad || !vpush(st, &sp, MLB_WASM_I32)) goto done; break;
            case 0x42: (void)rd_sleb(&c, 64); if (c.bad || !vpush(st, &sp, MLB_WASM_I64)) goto done; break;
            case 0x43: { const uint8_t *p; if (!rd_bytes(&c, 4, &p) || !vpush(st, &sp, MLB_WASM_F32)) goto done; break; }
            case 0x44: { const uint8_t *p; if (!rd_bytes(&c, 8, &p) || !vpush(st, &sp, MLB_WASM_F64)) goto done; break; }
            case 0xc0: case 0xc1:
                if (!vpop(st, &sp, cur, MLB_WASM_I32, NULL) || !vpush(st, &sp, MLB_WASM_I32)) goto done;
                break;
            case 0xc2: case 0xc3: case 0xc4:
                if (!vpop(st, &sp, cur, MLB_WASM_I64, NULL) || !vpush(st, &sp, MLB_WASM_I64)) goto done;
                break;
            default:
                if (op >= 0x45 && op <= 0x66) {
                    uint8_t input = (op <= 0x4f) ? MLB_WASM_I32 : (op <= 0x5a) ? MLB_WASM_I64 : (op <= 0x60) ? MLB_WASM_F32 : MLB_WASM_F64;
                    if (op == 0x45 || op == 0x50) {
                        input = op == 0x45 ? MLB_WASM_I32 : MLB_WASM_I64;
                        if (!vpop(st, &sp, cur, input, NULL)) goto done;
                    } else if (!vpop(st, &sp, cur, input, NULL) || !vpop(st, &sp, cur, input, NULL)) goto done;
                    if (!vpush(st, &sp, MLB_WASM_I32)) goto done;
                } else if (op >= 0x67 && op <= 0x78) {
                    if (!vpop(st, &sp, cur, MLB_WASM_I32, NULL) || ((op >= 0x6a) && !vpop(st, &sp, cur, MLB_WASM_I32, NULL)) || !vpush(st, &sp, MLB_WASM_I32)) goto done;
                } else if (op >= 0x79 && op <= 0x8a) {
                    if (!vpop(st, &sp, cur, MLB_WASM_I64, NULL) || ((op >= 0x7c) && !vpop(st, &sp, cur, MLB_WASM_I64, NULL)) || !vpush(st, &sp, MLB_WASM_I64)) goto done;
                } else if (op >= 0x8b && op <= 0x98) {
                    if (!vpop(st, &sp, cur, MLB_WASM_F32, NULL) || ((op >= 0x92) && !vpop(st, &sp, cur, MLB_WASM_F32, NULL)) || !vpush(st, &sp, MLB_WASM_F32)) goto done;
                } else if (op >= 0x99 && op <= 0xa6) {
                    if (!vpop(st, &sp, cur, MLB_WASM_F64, NULL) || ((op >= 0xa0) && !vpop(st, &sp, cur, MLB_WASM_F64, NULL)) || !vpush(st, &sp, MLB_WASM_F64)) goto done;
                } else {
                    uint8_t in = 0, out = 0;
                    switch (op) {
                        case 0xa7: in=MLB_WASM_I64; out=MLB_WASM_I32; break;
                        case 0xa8: case 0xa9: in=MLB_WASM_F32; out=MLB_WASM_I32; break;
                        case 0xaa: case 0xab: in=MLB_WASM_F64; out=MLB_WASM_I32; break;
                        case 0xac: case 0xad: in=MLB_WASM_I32; out=MLB_WASM_I64; break;
                        case 0xae: case 0xaf: in=MLB_WASM_F32; out=MLB_WASM_I64; break;
                        case 0xb0: case 0xb1: in=MLB_WASM_F64; out=MLB_WASM_I64; break;
                        case 0xb2: case 0xb3: in=MLB_WASM_I32; out=MLB_WASM_F32; break;
                        case 0xb4: case 0xb5: in=MLB_WASM_I64; out=MLB_WASM_F32; break;
                        case 0xb6: in=MLB_WASM_F64; out=MLB_WASM_F32; break;
                        case 0xb7: case 0xb8: in=MLB_WASM_I32; out=MLB_WASM_F64; break;
                        case 0xb9: case 0xba: in=MLB_WASM_I64; out=MLB_WASM_F64; break;
                        case 0xbb: in=MLB_WASM_F32; out=MLB_WASM_F64; break;
                        case 0xbc: in=MLB_WASM_F32; out=MLB_WASM_I32; break;
                        case 0xbd: in=MLB_WASM_F64; out=MLB_WASM_I64; break;
                        case 0xbe: in=MLB_WASM_I32; out=MLB_WASM_F32; break;
                        case 0xbf: in=MLB_WASM_I64; out=MLB_WASM_F64; break;
                        default: goto done;
                    }
                    if (!vpop(st, &sp, cur, in, NULL) || !vpush(st, &sp, out)) goto done;
                }
                break;
        }
        if (c.bad) goto done;
    }
    ok = ended;
done:
    free(st);
    return ok;
}

static void free_module(MlbWasmModule*m){if(!m)return;for(uint32_t i=0;i<m->nfuncs;i++)free(m->funcs[i].locals);for(uint32_t i=0;i<m->ntypes;i++){free(m->types[i].params);free(m->types[i].result_types);}for(uint32_t i=0;i<m->nexports;i++)free(m->exports[i].name);for(uint32_t i=0;i<m->nimports;i++){free(m->imports[i].module_name);free(m->imports[i].name);}for(uint32_t i=0;i<m->ndata;i++)free(m->data[i].bytes);for(uint32_t i=0;i<m->nelements;i++)free(m->elements[i].funcs);for(uint32_t i=0;i<m->ncustom;i++)free(m->custom[i].name);free(m->binary);free(m->types);free(m->funcs);free(m->func_types);free(m->globals);free(m->exports);free(m->imports);free(m->data);free(m->elements);free(m->custom);free(m);}
MlbWasmModule *mlb_wasm_rt_module_new(const uint8_t*bytes,size_t length,char*error,size_t ec){MlbWasmModule*m=NULL;const uint8_t magic[8]={0,97,115,109,1,0,0,0};if(error&&ec)error[0]=0;if(!bytes||length<8||memcmp(bytes,magic,8))return NULL;m=calloc(1,sizeof(*m));if(!m)return NULL;m->binary=malloc(length);if(!m->binary){free(m);return NULL;}memcpy(m->binary,bytes,length);m->binary_len=length;
    Cur top={m->binary,length,8,0};unsigned seen=0;int last_rank=0;uint32_t declared_funcs=0,code_funcs=0;int got_code=0;const char *unsupported_reason="Module is validly framed but outside the native runtime subset";
    while(top.pos<top.n&&!top.bad){uint8_t sid=rd8(&top);uint32_t slen;if(!rd_u32(&top,&slen)||slen>top.n-top.pos)goto unsupported;size_t end=top.pos+slen;Cur c={top.p+top.pos,slen,0,0};if(sid){int rank=sid==1?1:sid==2?2:sid==3?3:sid==4?4:sid==5?5:sid==6?6:sid==7?7:sid==8?8:sid==9?9:sid==12?10:sid==10?11:sid==11?12:0;if(!rank||(seen&(1u<<sid))||rank<=last_rank)goto unsupported;seen|=1u<<sid;last_rank=rank;}
        switch(sid){
        case 0:{char*name=NULL;if(!rd_name(&c,&name))goto unsupported;WCustom*q=realloc(m->custom,(m->ncustom+1u)*sizeof(*q));if(!q){free(name);goto oom;}m->custom=q;m->custom[m->ncustom++]=(WCustom){name,c.p+c.pos,c.n-c.pos};c.pos=c.n;break;}
        case 1:{uint32_t n;if(!rd_u32(&c,&n)||n>WASM_MAX_TYPES)goto unsupported;m->types=calloc(n?n:1,sizeof(*m->types));if(!m->types)goto oom;m->ntypes=n;for(uint32_t i=0;i<n;i++){if(rd8(&c)!=0x60)goto unsupported;uint32_t np;if(!rd_u32(&c,&np)||np>32)goto unsupported;WType*t=&m->types[i];t->nparams=np;t->params=malloc(np?np:1);if(!t->params)goto oom;for(uint32_t j=0;j<np;j++){t->params[j]=rd8(&c);if(!valid_valtype(t->params[j]))goto unsupported;}uint32_t nr;if(!rd_u32(&c,&nr)||nr>WASM_MAX_RESULTS)goto unsupported;t->nresults=nr;t->has_result=nr!=0;if(nr){t->result_types=malloc((size_t)nr);if(!t->result_types)goto oom;for(uint32_t j=0;j<nr;j++){t->result_types[j]=rd8(&c);if(!valid_valtype(t->result_types[j]))goto unsupported;}t->result=t->result_types[0];}}break;}
        case 2:{uint32_t n;if(!rd_u32(&c,&n)||n>1024)goto unsupported;if(n){m->imports=calloc(n,sizeof(*m->imports));if(!m->imports)goto oom;m->nimports=n;}for(uint32_t j=0;j<n;j++){WImport*im=&m->imports[j];if(!rd_name(&c,&im->module_name)||!rd_name(&c,&im->name))goto unsupported;im->kind=rd8(&c);if(im->kind==MLB_WASM_FUNC){if(!rd_u32(&c,&im->type)||im->type>=m->ntypes)goto unsupported;WFunc*q=realloc(m->funcs,(size_t)(m->nfuncs+1)*sizeof(*q));uint32_t*tq=realloc(m->func_types,(size_t)(m->nfuncs+1)*sizeof(*tq));if(!q||!tq){if(q)m->funcs=q;if(tq)m->func_types=tq;goto oom;}m->funcs=q;m->func_types=tq;WFunc*f=&m->funcs[m->nfuncs];memset(f,0,sizeof(*f));f->type=im->type;f->is_import=1;f->import_index=m->nimport_funcs;m->func_types[m->nfuncs]=im->type;m->nfuncs++;m->nimport_funcs++;}
            else if(im->kind==MLB_WASM_MEMORY){if(m->has_memory||!parse_limits(&c,&im->min,&im->max,&im->has_max))goto unsupported;m->has_memory=1;m->has_memory_import=1;m->memory_import_index=j;m->min_pages=im->min;m->max_pages=im->max;m->has_memory_max=im->has_max;}
            else if(im->kind==MLB_WASM_GLOBAL){im->value_type=rd8(&c);im->mut=rd8(&c);if(!valid_valtype(im->value_type)||im->mut>1||m->nglobals>=4096)goto unsupported;WGlobal*q=realloc(m->globals,(size_t)(m->nglobals+1)*sizeof(*q));if(!q)goto oom;m->globals=q;WGlobal*g=&m->globals[m->nglobals++];memset(g,0,sizeof(*g));g->type=im->value_type;g->mut=im->mut;g->heap_owned=0;im->type=m->nglobals-1;m->nimport_globals++;}
            else if(im->kind==MLB_WASM_TABLE){uint8_t et=rd8(&c);uint32_t flags;if(m->has_table||!valid_ref_type(et)||!rd_u32(&c,&flags)||flags>1||!rd_u32(&c,&im->min))goto unsupported;im->value_type=et;im->has_max=(int)(flags&1);im->max=im->min;if(im->has_max&&!rd_u32(&c,&im->max))goto unsupported;if(im->min>WASM_MAX_TABLE_ELEMS||im->max>WASM_MAX_TABLE_ELEMS||(im->has_max&&im->max<im->min))goto unsupported;m->has_table=1;m->has_table_import=1;m->table_import_index=j;m->table_min=im->min;m->table_max=im->max;m->table_max=im->max;m->table_type=et;m->has_table_max=im->has_max;}
            else {unsupported_reason="Native runtime supports only MVP funcref table imports";goto unsupported;}}
        break;}
        case 3:{if(!rd_u32(&c,&declared_funcs)||declared_funcs>WASM_MAX_FUNCS||m->nfuncs+declared_funcs>WASM_MAX_FUNCS)goto unsupported;m->defined_funcs=declared_funcs;uint32_t total=m->nfuncs+declared_funcs;WFunc*fq=realloc(m->funcs,(size_t)(total?total:1)*sizeof(*fq));uint32_t*tq=realloc(m->func_types,(size_t)(total?total:1)*sizeof(*tq));if(!fq||!tq){if(fq)m->funcs=fq;if(tq)m->func_types=tq;goto oom;}m->funcs=fq;m->func_types=tq;if(declared_funcs)memset(m->funcs+m->nfuncs,0,(size_t)declared_funcs*sizeof(*m->funcs));for(uint32_t i=0;i<declared_funcs;i++){uint32_t ty;if(!rd_u32(&c,&ty)||ty>=m->ntypes)goto unsupported;m->funcs[m->nfuncs+i].type=ty;m->func_types[m->nfuncs+i]=ty;}m->nfuncs=total;break;}
        case 4:{uint32_t n;if(!rd_u32(&c,&n)||n>1||(m->has_table_import&&n))goto unsupported;if(n){uint8_t et=rd8(&c);uint32_t min,max;int hm;if(!valid_ref_type(et))goto unsupported;uint32_t flags;if(!rd_u32(&c,&flags)||flags>1||!rd_u32(&c,&min))goto unsupported;hm=(int)(flags&1);max=min;if(hm&&!rd_u32(&c,&max))goto unsupported;if(min>WASM_MAX_TABLE_ELEMS||max>WASM_MAX_TABLE_ELEMS||(hm&&max<min))goto unsupported;m->has_table=1;m->table_min=min;m->table_max=max;m->table_type=et;m->has_table_max=hm;}break;}
        case 5:{uint32_t n;if(!rd_u32(&c,&n)||n>1||(m->has_memory_import&&n))goto unsupported;if(n){if(m->has_memory||!parse_limits(&c,&m->min_pages,&m->max_pages,&m->has_memory_max))goto unsupported;m->has_memory=1;}break;}
        case 6:{uint32_t n;if(!rd_u32(&c,&n)||n>4096||m->nglobals+n>4096)goto unsupported;WGlobal*q=realloc(m->globals,(size_t)(m->nglobals+n?n+m->nglobals:1)*sizeof(*q));if(!q)goto oom;m->globals=q;uint32_t base=m->nglobals;m->nglobals+=n;if(n)memset(m->globals+base,0,(size_t)n*sizeof(*m->globals));for(uint32_t i=0;i<n;i++){WGlobal*g=&m->globals[base+i];g->type=rd8(&c);g->mut=rd8(&c);if(!valid_valtype(g->type)||g->mut>1)goto unsupported;uint8_t t;uint64_t v;if(!parse_const_expr(&c,&t,&v)||t!=g->type)goto unsupported;g->bits=v;g->heap_owned=0;}break;}
        case 7:{uint32_t n;if(!rd_u32(&c,&n)||n>WASM_MAX_EXPORTS)goto unsupported;m->exports=calloc(n?n:1,sizeof(*m->exports));if(!m->exports)goto oom;m->nexports=n;for(uint32_t i=0;i<n;i++){WExport*x=&m->exports[i];if(!rd_name(&c,&x->name))goto unsupported;x->kind=rd8(&c);if(x->kind>3||!rd_u32(&c,&x->index))goto unsupported;}break;}
        case 8:if(!rd_u32(&c,&m->start_func))goto unsupported;m->start_seen=1;break;
        case 9:{
            uint32_t n;if(!rd_u32(&c,&n)||n>65536)goto unsupported;
            if(n){m->elements=calloc(n,sizeof(*m->elements));if(!m->elements)goto oom;m->nelements=n;}
            for(uint32_t j=0;j<n;j++){
                uint32_t flags;if(!rd_u32(&c,&flags)||flags>7)goto unsupported;
                WElem*x=&m->elements[j];x->mode=0;x->ref_type=0x70;
                int expression_form = flags >= 4;
                if(flags==0 || flags==4){
                    if(!m->has_table)goto unsupported;
                    uint8_t et;uint64_t offset;if(!parse_const_expr(&c,&et,&offset)||et!=MLB_WASM_I32)goto unsupported;x->offset=(uint32_t)offset;x->mode=0;
                    /* Binary element-segment flag 4 is active table 0 with
                     * expression elements; its reference type is implicitly funcref
                     * and is NOT encoded between the offset and vector count. */
                    if(flags==4){x->ref_type=MLB_WASM_FUNCREF;if(x->ref_type!=m->table_type)goto unsupported;}
                }else if(flags==1 || flags==5){
                    x->mode=1;
                    if(flags==1){if(rd8(&c)!=0||c.bad)goto unsupported;}
                    else {x->ref_type=rd8(&c);if(c.bad||!valid_ref_type(x->ref_type))goto unsupported;}
                }else if(flags==2 || flags==6){
                    uint32_t tableidx;if(!rd_u32(&c,&tableidx)||tableidx!=0||!m->has_table)goto unsupported;
                    uint8_t et;uint64_t offset;if(!parse_const_expr(&c,&et,&offset)||et!=MLB_WASM_I32)goto unsupported;x->offset=(uint32_t)offset;
                    if(flags==2){if(rd8(&c)!=0||c.bad)goto unsupported;}
                    else {x->ref_type=rd8(&c);if(c.bad||!valid_ref_type(x->ref_type)||x->ref_type!=m->table_type)goto unsupported;}
                    x->mode=0;
                }else if(flags==3 || flags==7){
                    x->mode=2;
                    if(flags==3){if(rd8(&c)!=0||c.bad)goto unsupported;}
                    else {x->ref_type=rd8(&c);if(c.bad||!valid_ref_type(x->ref_type))goto unsupported;}
                }
                if(x->mode==0 && x->ref_type!=m->table_type)goto unsupported;
                uint32_t count;if(!rd_u32(&c,&count)||count>WASM_MAX_FUNCS)goto unsupported;
                x->count=count;x->funcs=malloc(count?count*sizeof(uint32_t):1);if(!x->funcs)goto oom;
                for(uint32_t k=0;k<count;k++){
                    if(expression_form){uint8_t et=0;if(!parse_elem_expr(&c,&x->funcs[k],&et)||et!=x->ref_type)goto unsupported;}
                    else if(!rd_u32(&c,&x->funcs[k]))goto unsupported;
                }
            }break;}
        case 10:{uint32_t n;if(!rd_u32(&c,&n)||n!=m->defined_funcs)goto unsupported;code_funcs=n;got_code=1;for(uint32_t i=0;i<n;i++){uint32_t size;if(!rd_u32(&c,&size)||size>c.n-c.pos)goto unsupported;Cur body={c.p+c.pos,size,0,0};c.pos+=size;uint32_t groups;if(!rd_u32(&body,&groups)||groups>64)goto unsupported;uint32_t nl=0;uint8_t tmp[WASM_MAX_LOCALS];for(uint32_t g=0;g<groups;g++){uint32_t count;uint8_t type;if(!rd_u32(&body,&count))goto unsupported;type=rd8(&body);if(!valid_localtype(type)||count>WASM_MAX_LOCALS-nl)goto unsupported;for(uint32_t k=0;k<count;k++)tmp[nl++]=type;}if(body.bad||!scan_code(body.p+body.pos,body.n-body.pos,&m->needs_data_count))goto unsupported;WFunc*fn=&m->funcs[m->nimport_funcs+i];fn->nlocals=nl;fn->locals=malloc(nl?nl:1);if(!fn->locals)goto oom;memcpy(fn->locals,tmp,nl);fn->code=body.p+body.pos;fn->code_len=body.n-body.pos;}break;}
        case 11:{uint32_t n;if(!rd_u32(&c,&n)||n>65536)goto unsupported;m->data=calloc(n?n:1,sizeof(*m->data));if(!m->data)goto oom;m->ndata=n;for(uint32_t i=0;i<n;i++){uint32_t mode;if(!rd_u32(&c,&mode)||mode>2)goto unsupported;WData*d=&m->data[i];d->mode=(uint8_t)mode;if(mode==0||mode==2){if(mode==2){uint32_t memidx;if(!rd_u32(&c,&memidx)||memidx!=0)goto unsupported;}uint8_t t;uint64_t off;if(!parse_const_expr(&c,&t,&off)||t!=MLB_WASM_I32)goto unsupported;d->offset=(uint32_t)off;}uint32_t size;if(!rd_u32(&c,&size)||size>c.n-c.pos)goto unsupported;d->length=size;d->bytes=malloc(size?size:1);if(!d->bytes)goto oom;memcpy(d->bytes,c.p+c.pos,size);c.pos+=size;}break;}
        case 12: {uint32_t n;if(m->has_data_count||!rd_u32(&c,&n)||n>65536)goto unsupported;m->has_data_count=1;m->data_count=n;break;}
        default:goto unsupported;}
        if(c.bad||c.pos!=c.n)goto unsupported;
        top.pos=end;
    }
    if(top.bad||(!got_code&&m->defined_funcs)||declared_funcs!=code_funcs)goto unsupported;
    if((m->has_data_count&&m->data_count!=m->ndata)||(m->needs_data_count&&!m->has_data_count))goto unsupported;
    for(uint32_t i=0;i<m->nexports;i++){
        WExport*x=&m->exports[i];
        for(uint32_t j=0;j<i;j++) if(!strcmp(x->name,m->exports[j].name)) goto unsupported;
        if(x->kind==MLB_WASM_FUNC&&x->index>=m->nfuncs)goto unsupported;
        if(x->kind==MLB_WASM_TABLE&&(!m->has_table||x->index!=0))goto unsupported;
        if(x->kind==MLB_WASM_MEMORY&&(!m->has_memory||x->index!=0))goto unsupported;
        if(x->kind==MLB_WASM_GLOBAL&&x->index>=m->nglobals)goto unsupported;
    }
    if(m->start_seen){
        if(m->start_func>=m->nfuncs)goto unsupported;
        WType *st=&m->types[m->funcs[m->start_func].type];
        if(st->nparams!=0||st->nresults!=0)goto unsupported;
    }
    for(uint32_t d=0;d<m->ndata;d++) if(m->data[d].mode!=1&&!m->has_memory) goto unsupported;
    for(uint32_t j=0;j<m->nelements;j++) for(size_t k=0;k<m->elements[j].count;k++)
        if(m->elements[j].funcs[k]!=UINT32_MAX && m->elements[j].funcs[k]>=m->nfuncs) goto unsupported;
    for(uint32_t i=m->nimport_funcs;i<m->nfuncs;i++) if(!validate_func_types(m,&m->funcs[i])) goto unsupported;
    return m;
unsupported: if(error&&ec)snprintf(error,ec,"%s",unsupported_reason);free_module(m);return NULL;
oom: if(error&&ec)snprintf(error,ec,"Out of memory compiling native WebAssembly module");free_module(m);return NULL;}
void mlb_wasm_rt_module_free(MlbWasmModule*m){free_module(m);}
size_t mlb_wasm_rt_export_count(const MlbWasmModule*m){return m?m->nexports:0;}
size_t mlb_wasm_rt_import_count(const MlbWasmModule*m){return m?m->nimports:0;}
int mlb_wasm_rt_import_info(const MlbWasmModule*m,size_t i,const char**mod,const char**name,int*kind,uint32_t*fi){if(!m||i>=m->nimports)return 0;const WImport*x=&m->imports[i];if(mod)*mod=x->module_name;if(name)*name=x->name;if(kind)*kind=x->kind;if(fi)*fi=x->kind==MLB_WASM_FUNC?x->type:0;return 1;}
int mlb_wasm_rt_export_info(const MlbWasmModule*m,size_t i,const char**name,int*kind,uint32_t*idx){if(!m||i>=m->nexports)return 0;if(name)*name=m->exports[i].name;if(kind)*kind=m->exports[i].kind;if(idx)*idx=m->exports[i].index;return 1;}
int mlb_wasm_rt_find_export(const MlbWasmModule*m,const char*name,int*kind,uint32_t*idx){if(!m||!name)return 0;for(uint32_t i=0;i<m->nexports;i++)if(!strcmp(m->exports[i].name,name)){if(kind)*kind=m->exports[i].kind;if(idx)*idx=m->exports[i].index;return 1;}return 0;}
int mlb_wasm_rt_func_signature_multi(const MlbWasmModule*m,uint32_t fi,const uint8_t**params,size_t*np,const uint8_t**results,size_t*nr){if(!m||fi>=m->nfuncs)return 0;WType*t=&m->types[m->funcs[fi].type];if(params)*params=t->params;if(np)*np=t->nparams;if(results)*results=t->result_types;if(nr)*nr=t->nresults;return 1;}
int mlb_wasm_rt_func_signature(const MlbWasmModule*m,uint32_t fi,const uint8_t**params,size_t*np,int*hr,uint8_t*rt){if(!m||fi>=m->nfuncs)return 0;WType*t=&m->types[m->funcs[fi].type];if(params)*params=t->params;if(np)*np=t->nparams;if(hr)*hr=t->has_result;if(rt)*rt=t->result;return 1;}
int mlb_wasm_rt_global_info(const MlbWasmModule*m,uint32_t gi,uint8_t*type,int*is_mutable){if(!m||gi>=m->nglobals)return 0;if(type)*type=m->globals[gi].type;if(is_mutable)*is_mutable=m->globals[gi].mut;return 1;}
int mlb_wasm_rt_global_get(const MlbWasmInstance*i,uint32_t gi,MlbWasmValue*out){if(!i||!out||gi>=i->nglobals||!i->globals[gi])return 0;out->type=i->globals[gi]->type;out->bits=i->globals[gi]->bits;return wasm_ref_value_normalize(out);}
int mlb_wasm_rt_global_set(MlbWasmInstance*i,uint32_t gi,MlbWasmValue value){if(!i||gi>=i->nglobals||!i->globals[gi]||!i->globals[gi]->mut||i->globals[gi]->type!=value.type)return 0;i->globals[gi]->bits=value.bits;return 1;}
static WTable *table_new_internal(uint32_t initial, int has_max, uint32_t maximum,
                                  uint32_t minimum, uint8_t element_type, char *error, size_t ec) {
    if (!valid_ref_type(element_type) || initial > WASM_MAX_TABLE_ELEMS || minimum > initial ||
        (has_max && (maximum < initial || maximum > WASM_MAX_TABLE_ELEMS))) {
        errf(error, ec, "Invalid WebAssembly table limits or element type"); return NULL;
    }
    WTable *t=calloc(1,sizeof(*t));
    if(!t){errf(error,ec,"Out of memory allocating WebAssembly table resource");return NULL;}
    t->entries=calloc(initial?initial:1,sizeof(*t->entries));
    if(!t->entries){free(t);errf(error,ec,"Out of memory allocating WebAssembly table entries");return NULL;}
    t->size=initial;t->min_size=minimum;t->has_max=has_max;t->element_type=element_type;
    t->max_size=has_max?maximum:WASM_MAX_TABLE_ELEMS;t->refs=1;t->heap_owned=1;
    if(element_type==MLB_WASM_FUNCREF){for(uint32_t n=0;n<initial;n++)t->entries[n].function_index=UINT32_MAX;}
    t->registry_next=g_wasm_tables;g_wasm_tables=t;
    return t;
}
MlbWasmTable *mlb_wasm_rt_table_new(uint32_t initial, int has_max, uint32_t maximum,
                                    char *error,size_t ec){return table_new_internal(initial,has_max,maximum,initial,MLB_WASM_FUNCREF,error,ec);}
MlbWasmTable *mlb_wasm_rt_table_new_typed(uint32_t initial,int has_max,uint32_t maximum,uint8_t element_type,char*error,size_t ec){return table_new_internal(initial,has_max,maximum,initial,element_type,error,ec);}
int mlb_wasm_rt_table_element_type(const MlbWasmTable*table,uint8_t*element_type){if(!table)return 0;if(element_type)*element_type=table->element_type;return 1;}
int mlb_wasm_rt_table_resource_get_externref(const MlbWasmTable*table,uint32_t index,uint64_t*value){if(!table||table->element_type!=MLB_WASM_EXTERNREF||index>=table->size||!value)return 0;*value=table->entries[index].external_ref;return 1;}
int mlb_wasm_rt_table_resource_set_externref(MlbWasmTable*table,uint32_t index,uint64_t value){if(!table||table->element_type!=MLB_WASM_EXTERNREF||index>=table->size)return 0;table->entries[index]=(WFuncRef){NULL,UINT32_MAX,value};return 1;}
int64_t mlb_wasm_rt_table_resource_grow_externref(MlbWasmTable*table,uint32_t delta,uint64_t value){
    if(!table||table->element_type!=MLB_WASM_EXTERNREF)return -1;
    uint32_t old=table->size;
    if(old>table->max_size||old>WASM_MAX_TABLE_ELEMS||delta>table->max_size-old||delta>WASM_MAX_TABLE_ELEMS-old)return -1;
    /* A zero-delta grow succeeds with the current size even at the maximum.
       Avoid realloc so this no-op cannot spuriously fail under memory pressure. */
    if(!delta)return old;
    uint32_t nn=old+delta;
    WFuncRef*q=realloc(table->entries,(size_t)(nn?nn:1)*sizeof(*q));
    if(!q)return -1;
    table->entries=q;
    for(uint32_t k=old;k<nn;k++)table->entries[k]=(WFuncRef){NULL,UINT32_MAX,value};
    table->size=nn;return old;
}
/* Return whether an opaque host reference is retained by a live native Global or
 * Table. The JS adapter uses this at safe points to release host-side GC roots.
 * Values on the interpreter operand/local stacks are protected by the adapter's
 * native-call depth guard and are deliberately not considered persistent here. */
int mlb_wasm_rt_externref_is_live(uint64_t handle) {
    if (!handle) return 0; /* null is represented as token zero */
    for (WGlobal *g = g_wasm_globals; g; g = g->registry_next)
        if (g->type == MLB_WASM_EXTERNREF && g->bits == handle) return 1;
    for (WTable *t = g_wasm_tables; t; t = t->registry_next) {
        if (t->element_type != MLB_WASM_EXTERNREF) continue;
        for (uint32_t n = 0; n < t->size; ++n)
            if (t->entries[n].external_ref == handle) return 1;
    }
    return 0;
}
size_t mlb_wasm_rt_instance_externref_count(const MlbWasmInstance *instance) {
    if (!instance) return 0;
    size_t count = 0;
    for (uint32_t n = 0; n < instance->nglobals; ++n)
        if (instance->globals[n] && instance->globals[n]->type == MLB_WASM_EXTERNREF) count++;
    if (instance->has_table && instance->table && instance->table->element_type == MLB_WASM_EXTERNREF)
        count += instance->table->size;
    return count;
}
int mlb_wasm_rt_instance_externref_at(const MlbWasmInstance *instance, size_t index, uint64_t *handle) {
    if (!instance || !handle) return 0;
    for (uint32_t n = 0; n < instance->nglobals; ++n) {
        WGlobal *g = instance->globals[n];
        if (g && g->type == MLB_WASM_EXTERNREF) {
            if (index == 0) { *handle = g->bits; return 1; }
            --index;
        }
    }
    if (instance->has_table && instance->table && instance->table->element_type == MLB_WASM_EXTERNREF) {
        if (index < instance->table->size) { *handle = instance->table->entries[index].external_ref; return 1; }
    }
    return 0;
}
void mlb_wasm_rt_table_retain(MlbWasmTable *table){if(table&&table->heap_owned&&table->refs!=UINT32_MAX)table->refs++;}
void mlb_wasm_rt_table_release(MlbWasmTable *table){if(!table||!table->heap_owned||!table->refs)return;if(--table->refs)return;WTable **p=&g_wasm_tables;while(*p&&*p!=table)p=&(*p)->registry_next;if(*p==table)*p=table->registry_next;free(table->entries);free(table);}
MlbWasmTable *mlb_wasm_rt_table_handle(MlbWasmInstance *instance){if(!instance||!instance->has_table||!instance->table)return NULL;mlb_wasm_rt_table_retain(instance->table);return instance->table;}
int mlb_wasm_rt_table_resource_size(const MlbWasmTable *table,uint32_t *size){if(!table||!size)return 0;*size=table->size;return 1;}
int mlb_wasm_rt_table_resource_get_ref(const MlbWasmTable *table,uint32_t index,MlbWasmInstance **function_instance,uint32_t *function_index){if(!table||table->element_type!=MLB_WASM_FUNCREF||index>=table->size||!function_index)return 0;WFuncRef r=wasm_function_ref_canonical(table->entries[index]);if(function_instance)*function_instance=r.instance;*function_index=r.instance?r.function_index:UINT32_MAX;return 1;}
int mlb_wasm_rt_table_resource_set_ref(MlbWasmTable *table,uint32_t index,MlbWasmInstance *function_instance,uint32_t function_index){if(!table||table->element_type!=MLB_WASM_FUNCREF||index>=table->size)return 0;if(function_instance&&(!function_instance->module||function_index>=function_instance->module->nfuncs))return 0;WFuncRef ref={function_instance,function_instance?function_index:UINT32_MAX,0};table->entries[index]=wasm_function_ref_canonical(ref);return 1;}
int64_t mlb_wasm_rt_table_resource_grow_ref(MlbWasmTable *table,uint32_t delta,MlbWasmInstance *function_instance,uint32_t function_index){
    if(!table||table->element_type!=MLB_WASM_FUNCREF||(function_instance&&(!function_instance->module||function_index>=function_instance->module->nfuncs)))return -1;
    uint32_t old=table->size;
    if(old>table->max_size||old>WASM_MAX_TABLE_ELEMS||delta>table->max_size-old||delta>WASM_MAX_TABLE_ELEMS-old)return -1;
    /* table.grow(0) must return the old size even at the maximum; do not ask
       the allocator to resize an unchanged allocation. */
    if(!delta)return old;
    uint32_t nn=old+delta;
    WFuncRef*q=realloc(table->entries,(size_t)(nn?nn:1)*sizeof(*q));
    if(!q)return -1;
    table->entries=q;
    WFuncRef v=wasm_function_ref_canonical((WFuncRef){function_instance,function_instance?function_index:UINT32_MAX,0});
    for(uint32_t k=old;k<nn;k++)table->entries[k]=v;
    table->size=nn;return old;
}
int mlb_wasm_rt_table_size(const MlbWasmInstance*i,uint32_t*size){return i&&i->has_table&&mlb_wasm_rt_table_resource_size(i->table,size);}
int mlb_wasm_rt_table_get_ref(const MlbWasmInstance*i,uint32_t index,MlbWasmInstance **function_instance,uint32_t *function_index){return i&&i->has_table&&mlb_wasm_rt_table_resource_get_ref(i->table,index,function_instance,function_index);}
int mlb_wasm_rt_table_get(const MlbWasmInstance*i,uint32_t index,uint32_t*function_index){MlbWasmInstance*target=NULL;if(!mlb_wasm_rt_table_get_ref(i,index,&target,function_index))return 0;if(target&&target!=i)return 0;return 1;}

static uint64_t wasm_ref_token_for(WFuncRef ref) {
    ref = wasm_function_ref_canonical(ref);
    if (!ref.instance || !ref.instance->module ||
        ref.function_index >= ref.instance->module->nfuncs) return 0;
    for (size_t n = 0; n < g_wasm_ref_handle_count; ++n) {
        WRefHandle *h = &g_wasm_ref_handles[n];
        if (h->active && h->ref.instance == ref.instance &&
            h->ref.function_index == ref.function_index)
            return ((uint64_t)h->generation << 32) | ((uint64_t)n + 1u);
    }
    size_t slot;
    if (g_wasm_ref_free_head) {
        slot = (size_t)g_wasm_ref_free_head - 1u;
        g_wasm_ref_free_head = g_wasm_ref_handles[slot].next_free;
    } else {
        if (g_wasm_ref_handle_count >= UINT32_MAX) return 0;
        if (g_wasm_ref_handle_count == g_wasm_ref_handle_capacity) {
            size_t next = g_wasm_ref_handle_capacity ? g_wasm_ref_handle_capacity * 2u : 64u;
            if (next < g_wasm_ref_handle_capacity || next > UINT32_MAX ||
                next > SIZE_MAX / sizeof(*g_wasm_ref_handles)) return 0;
            WRefHandle *p = realloc(g_wasm_ref_handles, next * sizeof(*p));
            if (!p) return 0;
            g_wasm_ref_handles = p;
            g_wasm_ref_handle_capacity = next;
        }
        slot = g_wasm_ref_handle_count++;
        memset(&g_wasm_ref_handles[slot], 0, sizeof(g_wasm_ref_handles[slot]));
        g_wasm_ref_handles[slot].generation = 1;
    }
    WRefHandle *h = &g_wasm_ref_handles[slot];
    if (!h->generation) h->generation = 1;
    h->ref = ref;
    h->next_free = 0;
    h->active = 1;
    return ((uint64_t)h->generation << 32) | ((uint64_t)slot + 1u);
}

static int wasm_ref_from_token(uint64_t token, WFuncRef *out) {
    if (!out) return 0;
    if (token == 0) { *out = (WFuncRef){NULL, UINT32_MAX, 0}; return 1; }
    uint32_t slot_plus_one = (uint32_t)token;
    uint32_t generation = (uint32_t)(token >> 32);
    if (!slot_plus_one || !generation || slot_plus_one > g_wasm_ref_handle_count) return 0;
    WRefHandle *h = &g_wasm_ref_handles[slot_plus_one - 1u];
    if (!h->active || h->generation != generation) return 0;
    *out = wasm_function_ref_canonical(h->ref);
    return out->instance && out->instance->module &&
           out->function_index < out->instance->module->nfuncs;
}
int mlb_wasm_rt_function_ref_value(MlbWasmInstance *instance, uint32_t function_index,
                                   MlbWasmValue *out) {
    if (!instance || !instance->module || !out || function_index >= instance->module->nfuncs) return 0;
    uint64_t token = wasm_ref_token_for((WFuncRef){instance, function_index, 0});
    if (!token) return 0;
    out->type = MLB_WASM_FUNCREF;
    out->bits = token;
    return 1;
}

int mlb_wasm_rt_function_ref_alias(MlbWasmInstance *instance, uint32_t function_index,
                                   MlbWasmInstance *target, uint32_t target_function_index) {
    if (!instance || !instance->module || !instance->function_aliases ||
        function_index >= instance->module->nimport_funcs || !target || !target->module ||
        target_function_index >= target->module->nfuncs) return 0;
    const uint8_t *ap=NULL,*ar=NULL,*bp=NULL,*br=NULL;
    size_t an=0,arn=0,bn=0,brn=0;
    if (!mlb_wasm_rt_func_signature_multi(instance->module,function_index,&ap,&an,&ar,&arn) ||
        !mlb_wasm_rt_func_signature_multi(target->module,target_function_index,&bp,&bn,&br,&brn) ||
        an!=bn || arn!=brn || (an && memcmp(ap,bp,an)!=0) ||
        (arn && memcmp(ar,br,arn)!=0)) return 0;
    WFuncRef resolved=wasm_function_ref_canonical((WFuncRef){target,target_function_index,0});
    if (!resolved.instance || !resolved.instance->module ||
        resolved.function_index>=resolved.instance->module->nfuncs ||
        (resolved.instance==instance && resolved.function_index==function_index)) return 0;
    instance->function_aliases[function_index]=resolved;
    /* References may have been materialized by a start function before the
     * embedder finished resolving imports. Their tokens remain valid, but now
     * point at the canonical target so later table/global reads converge. */
    for (size_t n=0;n<g_wasm_ref_handle_count;n++) {
        WRefHandle *h=&g_wasm_ref_handles[n];
        if (h->active && h->ref.instance==instance && h->ref.function_index==function_index)
            h->ref=resolved;
    }
    return 1;
}
int mlb_wasm_rt_table_set_ref(MlbWasmInstance*i,uint32_t index,MlbWasmInstance*target,uint32_t function_index){return i&&i->has_table&&mlb_wasm_rt_table_resource_set_ref(i->table,index,target,function_index);}
int mlb_wasm_rt_table_set(MlbWasmInstance*i,uint32_t index,uint32_t function_index){if(!i||!i->has_table||(function_index!=UINT32_MAX&&function_index>=i->module->nfuncs))return 0;return mlb_wasm_rt_table_set_ref(i,index,function_index==UINT32_MAX?NULL:i,function_index);}
int64_t mlb_wasm_rt_table_grow(MlbWasmInstance*i,uint32_t delta,uint32_t initial){if(!i||!i->has_table||(initial!=UINT32_MAX&&initial>=i->module->nfuncs))return -1;return mlb_wasm_rt_table_resource_grow_ref(i->table,delta,initial==UINT32_MAX?NULL:i,initial);}
static int mem_init(WMemory *mem, uint32_t min, uint32_t max, int hm) {
    if (!mem || min > WASM_MAX_PAGES || (hm && (max < min || max > WASM_MAX_PAGES))) return 0;
    memset(mem, 0, sizeof(*mem));
    mem->pages = min; mem->min_pages = min;
    mem->max_pages = hm ? max : WASM_MAX_PAGES; mem->has_max = hm;
    /* Native JS Memory.buffer objects point directly into this allocation.
       Reserve the allowed capacity once so grow never moves the backing store.
       With no declared maximum the engine's limit (1024 pages / 64 MiB) applies. */
    size_t capacity = (size_t)mem->max_pages * WASM_PAGE_SIZE;
    mem->data = malloc(capacity ? capacity : 1);
    if (!mem->data) return 0;
    size_t active = (size_t)min * WASM_PAGE_SIZE;
    if (active) memset(mem->data, 0, active);
    mem->refs = 1;
    return 1;
}
static void memory_release_internal(WMemory *mem) {
    if (!mem) return;
    if (mem->refs && --mem->refs == 0) { free(mem->data); free(mem); }
}
static void global_register(WGlobal *g) {
    if (!g || !g->heap_owned || g->registered) return;
    g->registry_next = g_wasm_globals;
    g_wasm_globals = g;
    g->registered = 1;
}
static void global_unregister(WGlobal *g) {
    if (!g || !g->registered) return;
    WGlobal **p = &g_wasm_globals;
    while (*p && *p != g) p = &(*p)->registry_next;
    if (*p == g) *p = g->registry_next;
    g->registry_next = NULL;
    g->registered = 0;
}
static void global_release_internal(WGlobal *g) {
    if (!g) return;
    if (g->heap_owned && g->refs && --g->refs == 0) {
        global_unregister(g);
        free(g);
    }
}
static const MlbWasmImportValue *find_resource_binding(const MlbWasmModule *m,
        const MlbWasmImportValue *bindings, size_t nbindings, uint32_t import_index, int kind) {
    if (!m || !bindings || import_index >= m->nimports) return NULL;
    const WImport *im = &m->imports[import_index];
    if (im->kind != kind) return NULL;
    for (size_t j=0; j<nbindings; j++)
        if (bindings[j].kind == kind && bindings[j].module_name && bindings[j].name &&
            !strcmp(bindings[j].module_name, im->module_name) && !strcmp(bindings[j].name, im->name))
            return &bindings[j];
    return NULL;
}
MlbWasmMemory *mlb_wasm_rt_memory_new(uint32_t initial, int has_max, uint32_t maximum,
                                      char *error, size_t ec) {
    if (error && ec) error[0]=0;
    if (initial > WASM_MAX_PAGES || (has_max && (maximum < initial || maximum > WASM_MAX_PAGES))) {
        errf(error, ec, "Invalid WebAssembly memory limits"); return NULL;
    }
    WMemory *mem=calloc(1,sizeof(*mem));
    if (!mem) { errf(error,ec,"Out of memory allocating WebAssembly memory resource"); return NULL; }
    if (!mem_init(mem,initial,maximum,has_max)) { free(mem); errf(error,ec,"Out of memory allocating WebAssembly linear memory"); return NULL; }
    return mem;
}
void mlb_wasm_rt_memory_retain(MlbWasmMemory *memory) { if (memory && memory->refs != UINT32_MAX) memory->refs++; }
void mlb_wasm_rt_memory_release(MlbWasmMemory *memory) { memory_release_internal(memory); }
uint8_t *mlb_wasm_rt_memory_resource_data(MlbWasmMemory *memory,size_t *length,uint32_t *pages,uint64_t *epoch) {
    if (!memory) return NULL;
    if (length) *length=(size_t)memory->pages*WASM_PAGE_SIZE;
    if (pages) *pages=memory->pages;
    if (epoch) *epoch=memory->epoch;
    return memory->data;
}
int64_t mlb_wasm_rt_memory_resource_grow(MlbWasmMemory *memory,uint32_t delta) {
    if (!memory) return -1;
    uint32_t old=memory->pages;
    if (old > memory->max_pages || old > WASM_MAX_PAGES ||
        delta > memory->max_pages-old || delta > WASM_MAX_PAGES-old) return -1;
    /* The base address is stable for external ArrayBuffers. Only newly visible
       pages are initialized; active length and epoch publish the growth. */
    if (!memory->data) return -1;
    if (delta) memset(memory->data+(size_t)old*WASM_PAGE_SIZE,0,(size_t)delta*WASM_PAGE_SIZE);
    memory->pages=old+delta; memory->epoch++;
    return old;
}
MlbWasmGlobal *mlb_wasm_rt_global_new(uint8_t type,int mutable_value,MlbWasmValue initial,
                                      char *error,size_t ec) {
    if (error && ec) error[0]=0;
    if (!valid_valtype(type) || (mutable_value!=0 && mutable_value!=1) || initial.type!=type) {
        errf(error,ec,"Invalid WebAssembly global type or initial value"); return NULL;
    }
    WGlobal *g=calloc(1,sizeof(*g));
    if (!g) { errf(error,ec,"Out of memory allocating WebAssembly global resource"); return NULL; }
    g->type=type; g->mut=(uint8_t)mutable_value; g->bits=initial.bits; g->refs=1; g->heap_owned=1;
    global_register(g);
    return g;
}
void mlb_wasm_rt_global_retain(MlbWasmGlobal *global) { if(global&&global->heap_owned&&global->refs!=UINT32_MAX)global->refs++; }
void mlb_wasm_rt_global_release(MlbWasmGlobal *global) { global_release_internal(global); }
int mlb_wasm_rt_global_resource_get(const MlbWasmGlobal *global,MlbWasmValue *out) {
    if (!global || !out) return 0;
    out->type=global->type; out->bits=global->bits;
    return wasm_ref_value_normalize(out);
}
int mlb_wasm_rt_global_resource_set(MlbWasmGlobal *global,MlbWasmValue value) {
    if (!global || !global->mut || global->type!=value.type || !wasm_ref_value_normalize(&value)) return 0;
    global->bits=value.bits;
    return 1;
}
MlbWasmMemory *mlb_wasm_rt_memory_handle(MlbWasmInstance *instance) {
    if(!instance||!instance->has_memory||!instance->memory)return NULL;
    mlb_wasm_rt_memory_retain(instance->memory);return instance->memory;
}
MlbWasmGlobal *mlb_wasm_rt_global_handle(MlbWasmInstance *instance,uint32_t gi) {
    if(!instance||gi>=instance->nglobals||!instance->globals[gi])return NULL;
    mlb_wasm_rt_global_retain(instance->globals[gi]);return instance->globals[gi];
}
MlbWasmInstance *mlb_wasm_rt_instance_new_with_imports_multi(MlbWasmModule*m,MlbWasmHostCall host_call,MlbWasmHostCallMulti host_call_multi,
        void*host_opaque,const MlbWasmImportValue*bindings,size_t nbindings,char*error,size_t ec) {
    if(error&&ec)error[0]=0;
    if(!m){errf(error,ec,"Missing WebAssembly module");return NULL;}
    MlbWasmInstance*i=calloc(1,sizeof(*i));
    if(!i){errf(error,ec,"Out of memory creating WASM instance");return NULL;}
    i->module=m;i->host_call=host_call;i->host_call_multi=host_call_multi;i->host_opaque=host_opaque;i->nglobals=m->nglobals;
    if(m->nimport_funcs){
        i->function_aliases=calloc(m->nimport_funcs,sizeof(*i->function_aliases));
        if(!i->function_aliases){free(i);errf(error,ec,"Out of memory creating WebAssembly function alias map");return NULL;}
    }
    i->globals=calloc(i->nglobals?i->nglobals:1,sizeof(*i->globals));
    if(!i->globals){free(i->function_aliases);free(i);errf(error,ec,"Out of memory creating WASM globals");return NULL;}
    for(uint32_t g=0;g<i->nglobals;g++){
        if(g<m->nimport_globals){
            const WImport *im=NULL;
            for(uint32_t z=0;z<m->nimports;z++)if(m->imports[z].kind==MLB_WASM_GLOBAL&&m->imports[z].type==g){im=&m->imports[z];break;}
            const MlbWasmImportValue *b=NULL;
            if(im)for(size_t z=0;z<nbindings;z++)if(bindings[z].kind==MLB_WASM_GLOBAL&&bindings[z].module_name&&bindings[z].name&&!strcmp(bindings[z].module_name,im->module_name)&&!strcmp(bindings[z].name,im->name)){b=&bindings[z];break;}
            if(!b||!b->value.global){mlb_wasm_rt_instance_free(i);errf(error,ec,"Missing imported WebAssembly global");return NULL;}
            WGlobal *ext=b->value.global;
            if(ext->type!=im->value_type||ext->mut!=im->mut){mlb_wasm_rt_instance_free(i);errf(error,ec,"Imported WebAssembly global type or mutability mismatch");return NULL;}
            mlb_wasm_rt_global_retain(ext);i->globals[g]=ext;
        }else{
            WGlobal *local=calloc(1,sizeof(*local));
            if(!local){mlb_wasm_rt_instance_free(i);errf(error,ec,"Out of memory allocating WebAssembly global");return NULL;}
            *local=m->globals[g];local->refs=1;local->heap_owned=1;local->registered=0;local->registry_next=NULL;global_register(local);i->globals[g]=local;
        }
    }
    i->data_dropped=calloc(m->ndata?m->ndata:1,sizeof(*i->data_dropped));
    if(!i->data_dropped){mlb_wasm_rt_instance_free(i);errf(error,ec,"Out of memory tracking data segments");return NULL;}
    for(uint32_t d=0;d<m->ndata;d++)if(m->data[d].mode!=1)i->data_dropped[d]=1;
    i->elem_dropped=calloc(m->nelements?m->nelements:1,sizeof(*i->elem_dropped));
    if(!i->elem_dropped){mlb_wasm_rt_instance_free(i);errf(error,ec,"Out of memory tracking element segments");return NULL;}
    for(uint32_t e=0;e<m->nelements;e++)if(m->elements[e].mode!=1)i->elem_dropped[e]=1;
    i->has_memory=m->has_memory;
    if(i->has_memory){
        if(m->has_memory_import){
            const WImport *im=&m->imports[m->memory_import_index];
            const MlbWasmImportValue *b=find_resource_binding(m,bindings,nbindings,m->memory_import_index,MLB_WASM_MEMORY);
            if(!b||!b->value.memory){mlb_wasm_rt_instance_free(i);errf(error,ec,"Missing imported WebAssembly memory");return NULL;}
            WMemory *ext=b->value.memory;
            if(ext->pages<im->min || ext->min_pages<im->min || (im->has_max&&(!ext->has_max||ext->max_pages>im->max))){mlb_wasm_rt_instance_free(i);errf(error,ec,"Imported WebAssembly memory limits are incompatible");return NULL;}
            mlb_wasm_rt_memory_retain(ext);i->memory=ext;
        }else{
            i->memory=calloc(1,sizeof(*i->memory));
            if(!i->memory||!mem_init(i->memory,m->min_pages,m->max_pages,m->has_memory_max)){free(i->memory);i->memory=NULL;mlb_wasm_rt_instance_free(i);errf(error,ec,"Out of memory allocating WebAssembly linear memory");return NULL;}
        }
    }
    /* Validate active data segments before mutating an imported/shared table.
       Instantiation must not leave partially applied state if a later segment
       or the start function fails. */
    for(uint32_t d=0;d<m->ndata;d++){
        WData*x=&m->data[d];if(x->mode==1)continue;
        size_t len=i->has_memory?(size_t)i->memory->pages*WASM_PAGE_SIZE:0;
        if(!i->has_memory||x->offset>len||x->length>len-x->offset){mlb_wasm_rt_instance_free(i);errf(error,ec,"WebAssembly data segment is out of bounds");return NULL;}
    }
    i->has_table=m->has_table;
    if(i->has_table){
        if(m->has_table_import){
            const WImport *im=&m->imports[m->table_import_index];
            const MlbWasmImportValue *b=find_resource_binding(m,bindings,nbindings,m->table_import_index,MLB_WASM_TABLE);
            if(!b||!b->value.table){mlb_wasm_rt_instance_free(i);errf(error,ec,"Missing imported WebAssembly table");return NULL;}
            WTable *ext=b->value.table;
            if(ext->element_type!=im->value_type||ext->size<im->min||ext->min_size<im->min||(im->has_max&&(!ext->has_max||ext->max_size>im->max))){mlb_wasm_rt_instance_free(i);errf(error,ec,"Imported WebAssembly table limits are incompatible");return NULL;}
            mlb_wasm_rt_table_retain(ext);i->table=ext;
        }else{
            i->table=table_new_internal(m->table_min,m->has_table_max,m->table_max,m->table_min,m->table_type,error,ec);
            if(!i->table){mlb_wasm_rt_instance_free(i);if(error&&ec&&!error[0])errf(error,ec,"Unable to allocate WebAssembly table");return NULL;}
        }
        for(uint32_t eidx=0;eidx<m->nelements;eidx++){
            WElem*x=&m->elements[eidx];
            for(size_t k=0;k<x->count;k++)if(x->funcs[k]!=UINT32_MAX && (x->ref_type!=MLB_WASM_FUNCREF||x->funcs[k]>=m->nfuncs)){mlb_wasm_rt_instance_free(i);errf(error,ec,"WebAssembly element references an invalid function or has a type mismatch");return NULL;}
            if(x->mode!=0)continue; /* passive/declarative segments are not applied at instantiation */
            if(x->offset>i->table->size||x->count>i->table->size-x->offset){mlb_wasm_rt_instance_free(i);errf(error,ec,"WebAssembly element segment is out of bounds");return NULL;}
            for(size_t k=0;k<x->count;k++){
                uint32_t fi=x->funcs[k];
                if(x->ref_type==MLB_WASM_EXTERNREF){i->table->entries[x->offset+k]=(WFuncRef){NULL,UINT32_MAX,0};}
                else i->table->entries[x->offset+k]=(WFuncRef){fi==UINT32_MAX?NULL:i,fi,0};
            }
        }
    }
    for(uint32_t d=0;d<m->ndata;d++){
        WData*x=&m->data[d];if(x->mode==1)continue;
        memcpy(i->memory->data+x->offset,x->bytes,x->length);
    }
    if(m->start_seen){MlbWasmValue r;int hr;char e[192];if(!mlb_wasm_rt_call(i,m->start_func,NULL,0,&r,&hr,e,sizeof e)){
        /* Side effects on imported/shared resources performed before a start
         * trap are observable to the embedder (including table.grow). Do not
         * transactionally roll back the shared table. */
        mlb_wasm_rt_instance_free(i);
        if(error&&ec)snprintf(error,ec,"start function trapped: %.160s",e);
        return NULL;
    }}
    return i;
}
MlbWasmInstance *mlb_wasm_rt_instance_new_with_imports(MlbWasmModule*m,MlbWasmHostCall host_call,void*host_opaque,const MlbWasmImportValue*bindings,size_t nbindings,char*error,size_t ec){return mlb_wasm_rt_instance_new_with_imports_multi(m,host_call,NULL,host_opaque,bindings,nbindings,error,ec);}
MlbWasmInstance *mlb_wasm_rt_instance_new_with_host(MlbWasmModule*m,MlbWasmHostCall host_call,void*host_opaque,char*error,size_t ec){return mlb_wasm_rt_instance_new_with_imports(m,host_call,host_opaque,NULL,0,error,ec);}
MlbWasmInstance *mlb_wasm_rt_instance_new(MlbWasmModule*m,char*e,size_t c){return mlb_wasm_rt_instance_new_with_imports(m,NULL,NULL,NULL,0,e,c);}
void mlb_wasm_rt_instance_free(MlbWasmInstance*i){if(!i)return;/* Tables and ref handles are borrowed resources; invalidate before the owner disappears. */for(WTable*t=g_wasm_tables;t;t=t->registry_next)for(uint32_t n=0;n<t->size;n++)if(t->entries[n].instance==i)t->entries[n]=(WFuncRef){NULL,UINT32_MAX,0};for(size_t n=0;n<g_wasm_ref_handle_count;n++){WRefHandle*h=&g_wasm_ref_handles[n];if(h->active&&h->ref.instance==i){h->ref=(WFuncRef){NULL,UINT32_MAX,0};h->active=0;if(h->generation<UINT32_MAX){h->generation++;h->next_free=g_wasm_ref_free_head;g_wasm_ref_free_head=(uint32_t)n+1u;}else h->next_free=0;}}if(i->globals){for(uint32_t g=0;g<i->nglobals;g++)global_release_internal(i->globals[g]);}free(i->globals);memory_release_internal(i->memory);mlb_wasm_rt_table_release(i->table);free(i->function_aliases);free(i->data_dropped);free(i->elem_dropped);free(i);}
static int pop(uint64_t*s,int*sp,uint64_t*v){if(*sp<=0)return 0;*v=s[--*sp];return 1;}static int push(uint64_t*s,int*sp,uint64_t v){if(*sp>=(int)WASM_MAX_STACK)return 0;s[(*sp)++]=v;return 1;}
static float bits_f32(uint64_t v){uint32_t b=(uint32_t)v;float f;memcpy(&f,&b,sizeof f);return f;}
static double bits_f64(uint64_t v){double d;memcpy(&d,&v,sizeof d);return d;}
static uint32_t f32_bits(float f){uint32_t b;memcpy(&b,&f,sizeof b);return b;}
static uint64_t f64_bits(double d){uint64_t b;memcpy(&b,&d,sizeof b);return b;}
static int wasm_trunc_i32(double x,int uns,uint64_t*out){if(!isfinite(x))return 0;long double t=truncl((long double)x);if(uns){if(t<0.0L||t>=4294967296.0L)return 0;*out=(uint32_t)t;}else{if(t< -2147483648.0L||t>=2147483648.0L)return 0;*out=(uint32_t)(int32_t)t;}return 1;}
static int wasm_trunc_i64(double x,int uns,uint64_t*out){if(!isfinite(x))return 0;long double t=truncl((long double)x);if(uns){if(t<0.0L||t>=18446744073709551616.0L)return 0;*out=(uint64_t)t;}else{if(t< -9223372036854775808.0L||t>=9223372036854775808.0L)return 0;*out=(uint64_t)(int64_t)t;}return 1;}
static int charge_bulk_fuel(uint64_t *fuel,size_t length){uint64_t cost=((uint64_t)length+63u)/64u;if(cost>*fuel)return 0;*fuel-=cost;return 1;}
static uint32_t runtime_ctrl_result_count(const WControl *q) {
    return q->result_types ? q->nresults : (q->has_result ? 1u : 0u);
}
static uint32_t runtime_ctrl_label_count(const WControl *q) {
    return q->kind == 0x03 ? q->nparams : runtime_ctrl_result_count(q);
}
static int runtime_finish_control(uint64_t *st, int *sp, const WControl *q) {
    uint32_t nr = runtime_ctrl_result_count(q);
    if (nr > WASM_MAX_RESULTS || *sp < q->stack_height + (int)nr) return 0;
    uint64_t values[WASM_MAX_RESULTS];
    for (uint32_t k = 0; k < nr; ++k) values[k] = st[*sp - (int)nr + (int)k];
    *sp = q->stack_height;
    for (uint32_t k = 0; k < nr; ++k) if (!push(st, sp, values[k])) return 0;
    return 1;
}
/* Returns 1 when branched, 0 for an operand-stack failure, -1 for bad depth. */
static int runtime_apply_branch(uint64_t *st, int *sp, WControl *controls,
                                unsigned *control_depth, uint32_t label,
                                const WType *function_type, size_t *pc, int *done) {
    if (label > *control_depth) return -1;
    if (label == *control_depth) {
        if (!function_type || *sp < (int)function_type->nresults) return 0;
        *done = 1;
        return 1;
    }
    unsigned target_index = *control_depth - 1u - label;
    WControl *q = &controls[target_index];
    uint32_t count = runtime_ctrl_label_count(q);
    if (count > WASM_MAX_RESULTS || *sp < q->stack_height + (int)count) return 0;
    uint64_t values[WASM_MAX_RESULTS];
    for (uint32_t k = 0; k < count; ++k) values[k] = st[*sp - (int)count + (int)k];
    *sp = q->stack_height;
    for (uint32_t k = 0; k < count; ++k) if (!push(st, sp, values[k])) return 0;
    *control_depth = target_index + 1u;
    *pc = q->kind == 0x03 ? q->body_pos : q->end_pos;
    return 1;
}

static int invoke_multi(MlbWasmInstance*i,uint32_t fi,const MlbWasmValue*args,size_t argc,MlbWasmValue*out,size_t out_capacity,size_t*out_count,unsigned depth,uint64_t*fuel){MlbWasmModule*m=i->module;if(out_count)*out_count=0;if(depth>WASM_MAX_CALL_DEPTH){snprintf(i->error,sizeof i->error,"call stack exhausted");return 0;}if(*fuel==0){snprintf(i->error,sizeof i->error,"instruction fuel exhausted");return 0;}(*fuel)--;if(fi>=m->nfuncs){snprintf(i->error,sizeof i->error,"call target out of range");return 0;}WFunc*f=&m->funcs[fi];WType*t=&m->types[f->type];if(argc!=t->nparams){snprintf(i->error,sizeof i->error,"incorrect number of WebAssembly arguments");return 0;}if(t->nresults>out_capacity||(t->nresults&&!out)){snprintf(i->error,sizeof i->error,"WebAssembly result buffer is too small");return 0;}for(uint32_t k=0;k<t->nparams;k++)if(!args||args[k].type!=t->params[k]){snprintf(i->error,sizeof i->error,"incorrect WebAssembly argument type");return 0;}if(f->is_import){if(i->host_call_multi){size_t got=0;if(!i->host_call_multi(i->host_opaque,f->import_index,args,argc,out,out_capacity,&got,i->error,sizeof i->error))return 0;if(got!=t->nresults){snprintf(i->error,sizeof i->error,"host callback result count does not match WebAssembly signature");return 0;}for(size_t k=0;k<got;k++)if(out[k].type!=t->result_types[k]){snprintf(i->error,sizeof i->error,"host callback result type does not match WebAssembly signature");return 0;}if(out_count)*out_count=got;return 1;}if(!i->host_call){snprintf(i->error,sizeof i->error,"WebAssembly function import is not bound");return 0;}if(t->nresults>1){snprintf(i->error,sizeof i->error,"multi-value host import callbacks are not supported by this embedder");return 0;}MlbWasmValue one={0,0};int one_has=0;if(!i->host_call(i->host_opaque,f->import_index,args,argc,&one,&one_has,i->error,sizeof i->error))return 0;if(one_has!=(t->nresults!=0)){snprintf(i->error,sizeof i->error,"host callback result count does not match WebAssembly signature");return 0;}if(one_has){if(one.type!=t->result_types[0]){snprintf(i->error,sizeof i->error,"host callback result type does not match WebAssembly signature");return 0;}out[0]=one;if(out_count)*out_count=1;}return 1;}uint32_t nl=t->nparams+f->nlocals;uint64_t*locals=calloc(nl?nl:1,sizeof(uint64_t));if(!locals){snprintf(i->error,sizeof i->error,"out of memory creating locals");return 0;}for(uint32_t k=0;k<t->nparams;k++){locals[k]=args[k].bits;}
    Cur c={f->code,f->code_len,0,0};uint64_t st[WASM_MAX_STACK];int sp=0;int done=0;WControl controls[WASM_MAX_CALL_DEPTH];unsigned control_depth=0;while(c.pos<c.n&&!done){if(*fuel==0){snprintf(i->error,sizeof i->error,"instruction fuel exhausted");free(locals);return 0;}(*fuel)--;uint8_t op=rd8(&c);uint64_t a=0,b=0,v=0;uint32_t idx=0,align=0,offset=0;switch(op){case 0x00:snprintf(i->error,sizeof i->error,"unreachable executed");free(locals);return 0;case 0x01:break;
        case 0x02:case 0x03:case 0x04:{
            WBlockType bt; const uint8_t *params=NULL,*results=NULL;uint32_t nparams=0,nresults=0;size_t ep,en;
            if(control_depth>=WASM_MAX_CALL_DEPTH||!read_block_type(&c,&bt)||
               !resolve_block_type(m,&bt,&params,&nparams,&results,&nresults)||
               nparams>WASM_MAX_RESULTS||nresults>WASM_MAX_RESULTS||
               !find_control_bounds(f->code,f->code_len,c.pos,op,&ep,&en))goto trap;
            uint64_t cond=1;if(op==0x04&&!pop(st,&sp,&cond))goto stackerr;
            if(sp<(int)nparams)goto stackerr;
            WControl*q=&controls[control_depth++];memset(q,0,sizeof(*q));
            q->kind=op;q->result_type=bt.result_type;q->has_result=bt.indexed?(nresults!=0):bt.has_result;
            q->param_types=params;q->nparams=nparams;q->result_types=bt.indexed?results:NULL;q->nresults=bt.indexed?nresults:0;
            q->stack_height=sp-(int)nparams;q->body_pos=c.pos;q->else_pos=ep;q->end_pos=en;
            if(op==0x04&&!cond){if(ep!=SIZE_MAX)c.pos=ep+1;else c.pos=en;}
            break;}
        case 0x05:{
            if(!control_depth||controls[control_depth-1].kind!=0x04||controls[control_depth-1].else_pos==SIZE_MAX)goto trap;
            WControl*q=&controls[control_depth-1];if(!runtime_finish_control(st,&sp,q))goto stackerr;c.pos=q->end_pos;break;}
        case 0x0b:if(control_depth){WControl*q=&controls[control_depth-1];if(!runtime_finish_control(st,&sp,q))goto stackerr;control_depth--;}else done=1;break;
        case 0x0c:case 0x0d:{
            if (!rd_u32(&c, &idx)) goto trap;
            int take = 1;
            if (op == 0x0d) { if (!pop(st, &sp, &a)) goto stackerr; take = a != 0; }
            if (take) { int br = runtime_apply_branch(st, &sp, controls, &control_depth, idx, t, &c.pos, &done); if (br < 0) goto trap; if (!br) goto stackerr; }
            break;}
        case 0x0e:{
            uint32_t count,label,chosen=0;if(!pop(st,&sp,&a))goto stackerr;if(!rd_u32(&c,&count)||count>WASM_MAX_STACK)goto trap;
            uint32_t selector=(uint32_t)a;for(uint32_t j=0;j<count;++j){if(!rd_u32(&c,&label))goto trap;if(selector==j)chosen=label;}
            if (!rd_u32(&c, &label)) goto trap;
            if (selector >= count) chosen = label;
            int br = runtime_apply_branch(st, &sp, controls, &control_depth, chosen, t, &c.pos, &done);
            if (br < 0) goto trap;
            if (!br) goto stackerr;
            break;}
        case 0x0f:done=1;break;case 0x1a:if(!pop(st,&sp,&a))goto stackerr;break;case 0x1b:{uint64_t cond,x,y;if(!pop(st,&sp,&cond)||!pop(st,&sp,&y)||!pop(st,&sp,&x))goto stackerr;if(!push(st,&sp,cond?x:y))goto stackerr;break;}
        case 0x10:{if(!rd_u32(&c,&idx)||idx>=m->nfuncs)goto trap;WType*ct=&m->types[m->funcs[idx].type];if(ct->nparams>64||sp<(int)ct->nparams)goto stackerr;MlbWasmValue ca[64],cr[WASM_MAX_RESULTS];size_t crn=0;for(int k=(int)ct->nparams-1;k>=0;k--){ca[k].bits=st[--sp];ca[k].type=ct->params[k];}if(!invoke_multi(i,idx,ca,ct->nparams,cr,WASM_MAX_RESULTS,&crn,depth+1,fuel)){free(locals);return 0;}for(size_t k=0;k<crn;k++)if(!push(st,&sp,cr[k].bits))goto stackerr;break;}
        case 0x11:{uint32_t typeidx,tableidx;if(!rd_u32(&c,&typeidx)||!rd_u32(&c,&tableidx)||typeidx>=m->ntypes||tableidx!=0||!i->has_table||i->table->element_type!=MLB_WASM_FUNCREF||!pop(st,&sp,&a))goto trap;uint32_t slot=(uint32_t)a;if((uint64_t)slot>=i->table->size)goto trap;WFuncRef fr=i->table->entries[slot];MlbWasmInstance*target=fr.instance;uint32_t target_fi=fr.function_index;if(!target||!target->module||target_fi>=target->module->nfuncs)goto trap;MlbWasmModule*tm=target->module;WType*want=&m->types[typeidx],*actual=&tm->types[tm->funcs[target_fi].type];if(want->nparams!=actual->nparams||want->nresults!=actual->nresults||memcmp(want->params,actual->params,want->nparams)!=0||(want->nresults&&memcmp(want->result_types,actual->result_types,want->nresults)!=0))goto trap;if(actual->nparams>64||sp<(int)actual->nparams)goto stackerr;MlbWasmValue ca[64],cr[WASM_MAX_RESULTS];size_t crn=0;for(int k=(int)actual->nparams-1;k>=0;k--){ca[k].bits=st[--sp];ca[k].type=actual->params[k];}if(!invoke_multi(target,target_fi,ca,actual->nparams,cr,WASM_MAX_RESULTS,&crn,depth+1,fuel)){if(target!=i)snprintf(i->error,sizeof i->error,"%.191s",target->error);free(locals);return 0;}for(size_t k=0;k<crn;k++)if(!push(st,&sp,cr[k].bits))goto stackerr;break;}
        case 0x20:if(!rd_u32(&c,&idx)||idx>=nl)goto trap;if(!push(st,&sp,locals[idx]))goto stackerr;break;case 0x21:if(!rd_u32(&c,&idx)||idx>=nl||!pop(st,&sp,&a))goto stackerr;locals[idx]=a;break;case 0x22:if(!rd_u32(&c,&idx)||idx>=nl||sp<1)goto stackerr;locals[idx]=st[sp-1];break;
        case 0x23:{
            if(!rd_u32(&c,&idx)||idx>=i->nglobals)goto trap;
            MlbWasmValue value={i->globals[idx]->type,i->globals[idx]->bits};
            if(!wasm_ref_value_normalize(&value))goto trap;
            if(!push(st,&sp,value.bits))goto stackerr;
            break;
        }
        case 0x24:if(!rd_u32(&c,&idx)||idx>=i->nglobals||!i->globals[idx]->mut||!pop(st,&sp,&a))goto trap;i->globals[idx]->bits=a;break;
        case 0x25:{ /* table.get: funcref values use canonical tokens; externref values are opaque host handles. */
            if(!rd_u32(&c,&idx)||idx!=0||!i->has_table||!pop(st,&sp,&a))goto trap;
            uint32_t slot=(uint32_t)a;if((uint64_t)slot>=i->table->size)goto trap;
            WFuncRef ref=i->table->entries[slot];
            uint64_t token=0;
            if(i->table->element_type==MLB_WASM_EXTERNREF)token=ref.external_ref;
            else {token=wasm_ref_token_for(ref);if(ref.instance&&!token){snprintf(i->error,sizeof i->error,"out of memory creating function reference");free(locals);return 0;}}
            if(!push(st,&sp,token))goto stackerr;
            break;
        }
        case 0x26:{ /* table.set stores the statically validated reference type. */
            if(!rd_u32(&c,&idx)||idx!=0||!i->has_table||!pop(st,&sp,&a)||!pop(st,&sp,&b))goto trap;
            uint32_t slot=(uint32_t)b;if((uint64_t)slot>=i->table->size)goto trap;
            if(i->table->element_type==MLB_WASM_EXTERNREF)i->table->entries[slot]=(WFuncRef){NULL,UINT32_MAX,a};
            else {WFuncRef ref;if(!wasm_ref_from_token(a,&ref))goto trap;i->table->entries[slot]=ref;}
            break;
        }
        case 0x28:case 0x29:case 0x2a:case 0x2b:case 0x2c:case 0x2d:case 0x2e:case 0x2f:case 0x30:case 0x31:case 0x32:case 0x33:case 0x34:case 0x35:{
            if(!pop(st,&sp,&a)||!rd_u32(&c,&align)||!rd_u32(&c,&offset)||!i->has_memory)goto trap;
            size_t width=(op==0x28||op==0x2a||op==0x34||op==0x35)?4:(op==0x29||op==0x2b)?8:(op==0x2e||op==0x2f||op==0x32||op==0x33)?2:(op>=0x30&&op<=0x31)||op==0x2c||op==0x2d?1:4;
            uint64_t addr=(uint32_t)a+(uint64_t)offset;size_t mlen=(size_t)i->memory->pages*WASM_PAGE_SIZE;if(addr>mlen||width>mlen-(size_t)addr)goto trap;
            v=0;for(size_t z=0;z<width;z++)v|=(uint64_t)i->memory->data[(size_t)addr+z]<<(8*z);
            int sign=(op==0x2c||op==0x2e||op==0x30||op==0x32||op==0x34);
            if(sign&&width<8&&(v&((uint64_t)1<<(width*8-1))))v|=~((((uint64_t)1)<<(width*8))-1);
            if(!push(st,&sp,v))goto stackerr;
            break;}
        case 0x36:case 0x37:case 0x38:case 0x39:case 0x3a:case 0x3b:case 0x3c:case 0x3d:case 0x3e:{
            if(!pop(st,&sp,&b)||!pop(st,&sp,&a)||!rd_u32(&c,&align)||!rd_u32(&c,&offset)||!i->has_memory)goto trap;
            size_t width=(op==0x36||op==0x38||op==0x3e)?4:(op==0x37||op==0x39)?8:(op==0x3b||op==0x3d)?2:1;
            uint64_t addr=(uint32_t)a+(uint64_t)offset;size_t mlen=(size_t)i->memory->pages*WASM_PAGE_SIZE;if(addr>mlen||width>mlen-(size_t)addr)goto trap;
            for(size_t z=0;z<width;z++)i->memory->data[(size_t)addr+z]=(uint8_t)(b>>(8*z));
            break;}
        case 0x3f:if(rd_u32(&c,&idx)==0||idx!=0||!i->has_memory)goto trap;if(!push(st,&sp,i->memory->pages))goto stackerr;break;case 0x40:{if(!rd_u32(&c,&idx)||idx!=0||!pop(st,&sp,&a)||!i->has_memory)goto trap;int64_t old=mlb_wasm_rt_memory_grow(i,(uint32_t)a);if(old<0)old=-1;if(!push(st,&sp,(uint32_t)old))goto stackerr;break;}
        case 0xfc: {
            uint32_t subop, dataidx, memidx;
            if (!rd_u32(&c, &subop)) goto trap;
            if (subop == 8) { /* memory.init: destination, source, length */
                if (!rd_u32(&c, &dataidx) || !rd_u32(&c, &memidx) || dataidx >= m->ndata || memidx != 0 || !i->has_memory || !pop(st, &sp, &v) || !pop(st, &sp, &b) || !pop(st, &sp, &a)) goto trap;
                WData *seg = &m->data[dataidx];
                size_t dst = (uint32_t)a, src = (uint32_t)b, len = (uint32_t)v;
                size_t mlen = (size_t)i->memory->pages * WASM_PAGE_SIZE;
                size_t seglen = i->data_dropped[dataidx] ? 0 : seg->length;
                if (src > seglen || len > seglen - src || dst > mlen || len > mlen - dst) goto trap;
                if(!charge_bulk_fuel(fuel,len)){snprintf(i->error,sizeof i->error,"instruction fuel exhausted by memory.init");free(locals);return 0;}
                if (len) memcpy(i->memory->data + dst, seg->bytes + src, len);
            } else if (subop == 9) { /* data.drop */
                if (!rd_u32(&c, &dataidx) || dataidx >= m->ndata) goto trap;
                i->data_dropped[dataidx] = 1;
            } else if (subop == 10) { /* memory.copy, including overlapping ranges */
                uint32_t dstmem, srcmem;
                if (!rd_u32(&c, &dstmem) || !rd_u32(&c, &srcmem) || dstmem != 0 || srcmem != 0 || !i->has_memory || !pop(st, &sp, &v) || !pop(st, &sp, &b) || !pop(st, &sp, &a)) goto trap;
                size_t dst = (uint32_t)a, src = (uint32_t)b, len = (uint32_t)v;
                size_t mlen = (size_t)i->memory->pages * WASM_PAGE_SIZE;
                if (dst > mlen || len > mlen - dst || src > mlen || len > mlen - src) goto trap;
                if(!charge_bulk_fuel(fuel,len)){snprintf(i->error,sizeof i->error,"instruction fuel exhausted by memory.copy");free(locals);return 0;}
                if (len) memmove(i->memory->data + dst, i->memory->data + src, len);
            } else if (subop == 11) { /* memory.fill */
                if (!rd_u32(&c, &memidx) || memidx != 0 || !i->has_memory || !pop(st, &sp, &v) || !pop(st, &sp, &b) || !pop(st, &sp, &a)) goto trap;
                size_t dst = (uint32_t)a, len = (uint32_t)v;
                size_t mlen = (size_t)i->memory->pages * WASM_PAGE_SIZE;
                if (dst > mlen || len > mlen - dst) goto trap;
                if(!charge_bulk_fuel(fuel,len)){snprintf(i->error,sizeof i->error,"instruction fuel exhausted by memory.fill");free(locals);return 0;}
                if (len) memset(i->memory->data + dst, (uint8_t)b, len);
            } else if (subop == 12) { /* table.init elemidx tableidx */
                uint32_t elemidx,tableidx;if(!rd_u32(&c,&elemidx)||!rd_u32(&c,&tableidx)||elemidx>=m->nelements||tableidx!=0||!i->has_table||!pop(st,&sp,&v)||!pop(st,&sp,&b)||!pop(st,&sp,&a))goto trap;
                WElem *seg=&m->elements[elemidx];size_t dst=(uint32_t)a,src=(uint32_t)b,len=(uint32_t)v;size_t seglen=i->elem_dropped[elemidx]?0:seg->count;
                if(seg->mode!=1||src>seglen||len>seglen-src||dst>i->table->size||len>i->table->size-dst)goto trap;
                if(!charge_bulk_fuel(fuel,len*sizeof(WFuncRef))){snprintf(i->error,sizeof i->error,"instruction fuel exhausted by table.init");free(locals);return 0;}
                for(size_t k=0;k<len;k++){
                    uint32_t fi=seg->funcs[src+k];
                    if(i->table->element_type==MLB_WASM_EXTERNREF){if(fi!=UINT32_MAX)goto trap;i->table->entries[dst+k]=(WFuncRef){NULL,UINT32_MAX,0};}
                    else {if(fi!=UINT32_MAX && fi>=m->nfuncs)goto trap;i->table->entries[dst+k]=(WFuncRef){fi==UINT32_MAX?NULL:i,fi,0};}
                }
            } else if (subop == 13) { /* elem.drop */
                if(!rd_u32(&c,&dataidx)||dataidx>=m->nelements)goto trap;
                i->elem_dropped[dataidx]=1;
            } else if (subop == 14) { /* table.copy; only one table index is implemented */
                uint32_t dsttab,srctab;if(!rd_u32(&c,&dsttab)||!rd_u32(&c,&srctab)||dsttab!=0||srctab!=0||!i->has_table||!pop(st,&sp,&v)||!pop(st,&sp,&b)||!pop(st,&sp,&a))goto trap;
                size_t dst=(uint32_t)a,src=(uint32_t)b,len=(uint32_t)v;if(dst>i->table->size||len>i->table->size-dst||src>i->table->size||len>i->table->size-src)goto trap;
                if(!charge_bulk_fuel(fuel,len*sizeof(WFuncRef))){snprintf(i->error,sizeof i->error,"instruction fuel exhausted by table.copy");free(locals);return 0;}
                if(len)memmove(i->table->entries+dst,i->table->entries+src,len*sizeof(WFuncRef));
            } else if (subop == 15) { /* table.grow: ref value then delta */
                if(!rd_u32(&c,&memidx)||memidx!=0||!i->has_table||!pop(st,&sp,&a)||!pop(st,&sp,&b))goto trap;
                WFuncRef ref={NULL,UINT32_MAX,0};if(i->table->element_type==MLB_WASM_EXTERNREF)ref.external_ref=b;else if(!wasm_ref_from_token(b,&ref))goto trap;
                uint32_t delta=(uint32_t)a,old_size=i->table->size;int64_t old=-1;
                if(old_size<=i->table->max_size&&old_size<=WASM_MAX_TABLE_ELEMS&&delta<=i->table->max_size-old_size&&delta<=WASM_MAX_TABLE_ELEMS-old_size){
                    if(!charge_bulk_fuel(fuel,(size_t)delta*sizeof(WFuncRef))){snprintf(i->error,sizeof i->error,"instruction fuel exhausted by table.grow");free(locals);return 0;}
                    if(delta==0)old=old_size;
                    else {uint32_t nn=old_size+delta;WFuncRef*q=realloc(i->table->entries,(size_t)nn*sizeof(*q));if(q){i->table->entries=q;for(uint32_t k=old_size;k<nn;k++)i->table->entries[k]=ref;i->table->size=nn;old=old_size;}}
                }
                if(!push(st,&sp,(uint32_t)old))goto stackerr;
            } else if (subop == 16) { /* table.size */
                if(!rd_u32(&c,&memidx)||memidx!=0||!i->has_table||!push(st,&sp,i->table->size))goto trap;
            } else if (subop == 17) { /* table.fill */
                if(!rd_u32(&c,&memidx)||memidx!=0||!i->has_table||!pop(st,&sp,&v)||!pop(st,&sp,&b)||!pop(st,&sp,&a))goto trap;
                size_t dst=(uint32_t)a,len=(uint32_t)v;if(dst>i->table->size||len>i->table->size-dst)goto trap;
                WFuncRef ref={NULL,UINT32_MAX,0};if(i->table->element_type==MLB_WASM_EXTERNREF)ref.external_ref=b;else if(!wasm_ref_from_token(b,&ref))goto trap;
                if(!charge_bulk_fuel(fuel,len*sizeof(WFuncRef))){snprintf(i->error,sizeof i->error,"instruction fuel exhausted by table.fill");free(locals);return 0;}
                for(size_t k=0;k<len;k++)i->table->entries[dst+k]=ref;
            } else goto trap;
            break;
        }
        case 0x50:if(!pop(st,&sp,&a)||!push(st,&sp,a==0))goto stackerr;break;
        case 0x51:case 0x52:case 0x53:case 0x54:case 0x55:case 0x56:case 0x57:case 0x58:case 0x59:case 0x5a:if(!pop(st,&sp,&b)||!pop(st,&sp,&a))goto stackerr;switch(op){case 0x51:v=a==b;break;case 0x52:v=a!=b;break;case 0x53:v=(int64_t)a<(int64_t)b;break;case 0x54:v=a<b;break;case 0x55:v=(int64_t)a>(int64_t)b;break;case 0x56:v=a>b;break;case 0x57:v=(int64_t)a<=(int64_t)b;break;case 0x58:v=a<=b;break;case 0x59:v=(int64_t)a>=(int64_t)b;break;default:v=a>=b;break;}if(!push(st,&sp,v))goto stackerr;break;
        case 0x5b:case 0x5c:case 0x5d:case 0x5e:case 0x5f:case 0x60:{if(!pop(st,&sp,&b)||!pop(st,&sp,&a))goto stackerr;float x=bits_f32(a),y=bits_f32(b);switch(op){case 0x5b:v=x==y;break;case 0x5c:v=x!=y;break;case 0x5d:v=x<y;break;case 0x5e:v=x>y;break;case 0x5f:v=x<=y;break;default:v=x>=y;break;}if(!push(st,&sp,v))goto stackerr;break;}
        case 0x61:case 0x62:case 0x63:case 0x64:case 0x65:case 0x66:{if(!pop(st,&sp,&b)||!pop(st,&sp,&a))goto stackerr;double x=bits_f64(a),y=bits_f64(b);switch(op){case 0x61:v=x==y;break;case 0x62:v=x!=y;break;case 0x63:v=x<y;break;case 0x64:v=x>y;break;case 0x65:v=x<=y;break;default:v=x>=y;break;}if(!push(st,&sp,v))goto stackerr;break;}
        case 0x8b:case 0x8c:case 0x8d:case 0x8e:case 0x8f:case 0x90:case 0x91:{if(!pop(st,&sp,&a))goto stackerr;float x=bits_f32(a),y;switch(op){case 0x8b:y=fabsf(x);break;case 0x8c:y=-x;break;case 0x8d:y=ceilf(x);break;case 0x8e:y=floorf(x);break;case 0x8f:y=truncf(x);break;case 0x90:y=nearbyintf(x);break;default:y=sqrtf(x);break;}if(!push(st,&sp,f32_bits(y)))goto stackerr;break;}
        case 0x92:case 0x93:case 0x94:case 0x95:case 0x96:case 0x97:case 0x98:{if(!pop(st,&sp,&b)||!pop(st,&sp,&a))goto stackerr;float x=bits_f32(a),y=bits_f32(b),z;switch(op){case 0x92:z=x+y;break;case 0x93:z=x-y;break;case 0x94:z=x*y;break;case 0x95:z=x/y;break;case 0x96:z=(isnan(x)||isnan(y))?NAN:fminf(x,y);break;case 0x97:z=(isnan(x)||isnan(y))?NAN:fmaxf(x,y);break;default:z=copysignf(x,y);break;}if(!push(st,&sp,f32_bits(z)))goto stackerr;break;}
        case 0x99:case 0x9a:case 0x9b:case 0x9c:case 0x9d:case 0x9e:case 0x9f:{if(!pop(st,&sp,&a))goto stackerr;double x=bits_f64(a),y;switch(op){case 0x99:y=fabs(x);break;case 0x9a:y=-x;break;case 0x9b:y=ceil(x);break;case 0x9c:y=floor(x);break;case 0x9d:y=trunc(x);break;case 0x9e:y=nearbyint(x);break;default:y=sqrt(x);break;}if(!push(st,&sp,f64_bits(y)))goto stackerr;break;}
        case 0xa0:case 0xa1:case 0xa2:case 0xa3:case 0xa4:case 0xa5:case 0xa6:{if(!pop(st,&sp,&b)||!pop(st,&sp,&a))goto stackerr;double x=bits_f64(a),y=bits_f64(b),z;switch(op){case 0xa0:z=x+y;break;case 0xa1:z=x-y;break;case 0xa2:z=x*y;break;case 0xa3:z=x/y;break;case 0xa4:z=(isnan(x)||isnan(y))?NAN:fmin(x,y);break;case 0xa5:z=(isnan(x)||isnan(y))?NAN:fmax(x,y);break;default:z=copysign(x,y);break;}if(!push(st,&sp,f64_bits(z)))goto stackerr;break;}
        case 0xa8:case 0xa9:case 0xaa:case 0xab:{if(!pop(st,&sp,&a))goto stackerr;double x=(op==0xa8||op==0xa9)?(double)bits_f32(a):bits_f64(a);if(!wasm_trunc_i32(x,op==0xa9||op==0xab,&v))goto trap;if(!push(st,&sp,v))goto stackerr;break;}
        case 0xae:case 0xaf:case 0xb0:case 0xb1:{if(!pop(st,&sp,&a))goto stackerr;double x=(op==0xae||op==0xaf)?(double)bits_f32(a):bits_f64(a);if(!wasm_trunc_i64(x,op==0xaf||op==0xb1,&v))goto trap;if(!push(st,&sp,v))goto stackerr;break;}
        case 0xb2:case 0xb3:case 0xb4:case 0xb5:case 0xb6:{if(!pop(st,&sp,&a))goto stackerr;float x;if(op==0xb2)x=(float)(int32_t)a;else if(op==0xb3)x=(float)(uint32_t)a;else if(op==0xb4)x=(float)(int64_t)a;else if(op==0xb5)x=(float)(uint64_t)a;else x=(float)bits_f64(a);if(!push(st,&sp,f32_bits(x)))goto stackerr;break;}
        case 0xb7:case 0xb8:case 0xb9:case 0xba:case 0xbb:{if(!pop(st,&sp,&a))goto stackerr;double x;if(op==0xb7)x=(double)(int32_t)a;else if(op==0xb8)x=(double)(uint32_t)a;else if(op==0xb9)x=(double)(int64_t)a;else if(op==0xba)x=(double)(uint64_t)a;else x=(double)bits_f32(a);if(!push(st,&sp,f64_bits(x)))goto stackerr;break;}
        case 0xbc:if(!pop(st,&sp,&a)||!push(st,&sp,(uint32_t)a))goto stackerr;break;
        case 0xbd:if(!pop(st,&sp,&a)||!push(st,&sp,a))goto stackerr;break;
        case 0xbe:if(!pop(st,&sp,&a)||!push(st,&sp,(uint32_t)a))goto stackerr;break;
        case 0xbf:if(!pop(st,&sp,&a)||!push(st,&sp,a))goto stackerr;break;
        case 0xc0:if(!pop(st,&sp,&a)||!push(st,&sp,(uint32_t)(((uint32_t)a^0x80u)-0x80u)))goto stackerr;break;
        case 0xc1:if(!pop(st,&sp,&a)||!push(st,&sp,(uint32_t)(((uint32_t)a^0x8000u)-0x8000u)))goto stackerr;break;
        case 0xc2:if(!pop(st,&sp,&a)||!push(st,&sp,(a^UINT64_C(0x80))-UINT64_C(0x80)))goto stackerr;break;
        case 0xc3:if(!pop(st,&sp,&a)||!push(st,&sp,(a^UINT64_C(0x8000))-UINT64_C(0x8000)))goto stackerr;break;
        case 0xc4:if(!pop(st,&sp,&a)||!push(st,&sp,(a^UINT64_C(0x80000000))-UINT64_C(0x80000000)))goto stackerr;break;
        case 0x67:if(!pop(st,&sp,&a)||!push(st,&sp,(uint32_t)a?__builtin_clz((uint32_t)a):32))goto stackerr;break;
        case 0x68:if(!pop(st,&sp,&a)||!push(st,&sp,(uint32_t)a?__builtin_ctz((uint32_t)a):32))goto stackerr;break;
        case 0x69:if(!pop(st,&sp,&a)||!push(st,&sp,__builtin_popcount((uint32_t)a)))goto stackerr;break;
        case 0x79:if(!pop(st,&sp,&a)||!push(st,&sp,a?__builtin_clzll(a):64))goto stackerr;break;
        case 0x7a:if(!pop(st,&sp,&a)||!push(st,&sp,a?__builtin_ctzll(a):64))goto stackerr;break;
        case 0x7b:if(!pop(st,&sp,&a)||!push(st,&sp,__builtin_popcountll(a)))goto stackerr;break;
        case 0x7c:case 0x7d:case 0x7e:case 0x7f:case 0x80:case 0x81:case 0x82:case 0x83:case 0x84:case 0x85:case 0x86:case 0x87:case 0x88:case 0x89:case 0x8a:if(!pop(st,&sp,&b)||!pop(st,&sp,&a))goto stackerr;switch(op){case 0x7c:v=a+b;break;case 0x7d:v=a-b;break;case 0x7e:v=a*b;break;case 0x7f:if(!b||((int64_t)a==INT64_MIN&&(int64_t)b==-1))goto trap;v=(uint64_t)((int64_t)a/(int64_t)b);break;case 0x80:if(!b)goto trap;v=a/b;break;case 0x81:if(!b)goto trap;v=((int64_t)a==INT64_MIN&&(int64_t)b==-1)?0:(uint64_t)((int64_t)a%(int64_t)b);break;case 0x82:if(!b)goto trap;v=a%b;break;case 0x83:v=a&b;break;case 0x84:v=a|b;break;case 0x85:v=a^b;break;case 0x86:v=a<<(b&63);break;case 0x87:v=(uint64_t)((int64_t)a>>(b&63));break;case 0x88:v=a>>(b&63);break;case 0x89:v=(a<<(b&63))|(a>>((64-(b&63))&63));break;default:v=(a>>(b&63))|(a<<((64-(b&63))&63));break;}if(!push(st,&sp,v))goto stackerr;break;
        case 0xd0:{uint8_t rt=rd8(&c);if(c.bad||!valid_ref_type(rt)||!push(st,&sp,0))goto trap;break;}
        case 0xd1:if(!pop(st,&sp,&a)||!push(st,&sp,a==0))goto stackerr;break;
        case 0xd2:{if(!rd_u32(&c,&idx)||idx>=m->nfuncs||!module_declares_function_ref(m,idx))goto trap;uint64_t token=wasm_ref_token_for((WFuncRef){i,idx,0});if(!token){snprintf(i->error,sizeof i->error,"out of memory creating function reference");free(locals);return 0;}if(!push(st,&sp,token))goto stackerr;break;}
        case 0x41:v=(uint32_t)(int32_t)rd_sleb(&c,32);if(c.bad||!push(st,&sp,v))goto stackerr;break;case 0x42:v=(uint64_t)rd_sleb(&c,64);if(c.bad||!push(st,&sp,v))goto stackerr;break;
        case 0xa7:if(!pop(st,&sp,&a)||!push(st,&sp,(uint32_t)a))goto stackerr;break;case 0xac:if(!pop(st,&sp,&a)||!push(st,&sp,(uint64_t)(int64_t)(int32_t)a))goto stackerr;break;case 0xad:if(!pop(st,&sp,&a)||!push(st,&sp,(uint32_t)a))goto stackerr;break;case 0x43:{const uint8_t*q;if(!rd_bytes(&c,4,&q))goto trap;v=(uint32_t)q[0]|(uint32_t)q[1]<<8|(uint32_t)q[2]<<16|(uint32_t)q[3]<<24;if(!push(st,&sp,v))goto stackerr;break;}case 0x44:{const uint8_t*q;if(!rd_bytes(&c,8,&q))goto trap;v=0;for(int z=7;z>=0;z--)v=(v<<8)|q[z];if(!push(st,&sp,v))goto stackerr;break;}
        case 0x45:if(!pop(st,&sp,&a)||!push(st,&sp,(uint32_t)a==0))goto stackerr;break;
        case 0x46:case 0x47:case 0x48:case 0x49:case 0x4a:case 0x4b:case 0x4c:case 0x4d:case 0x4e:case 0x4f:if(!pop(st,&sp,&b)||!pop(st,&sp,&a))goto stackerr;switch(op){case 0x46:v=(uint32_t)a==(uint32_t)b;break;case 0x47:v=(uint32_t)a!=(uint32_t)b;break;case 0x48:v=(int32_t)a<(int32_t)b;break;case 0x49:v=(uint32_t)a<(uint32_t)b;break;case 0x4a:v=(int32_t)a>(int32_t)b;break;case 0x4b:v=(uint32_t)a>(uint32_t)b;break;case 0x4c:v=(int32_t)a<=(int32_t)b;break;case 0x4d:v=(uint32_t)a<=(uint32_t)b;break;case 0x4e:v=(int32_t)a>=(int32_t)b;break;default:v=(uint32_t)a>=(uint32_t)b;break;}if(!push(st,&sp,v))goto stackerr;break;
        case 0x6a:case 0x6b:case 0x6c:case 0x6d:case 0x6e:case 0x6f:case 0x70:case 0x71:case 0x72:case 0x73:case 0x74:case 0x75:case 0x76:case 0x77:case 0x78:if(!pop(st,&sp,&b)||!pop(st,&sp,&a))goto stackerr;switch(op){case 0x6a:v=(uint32_t)a+(uint32_t)b;break;case 0x6b:v=(uint32_t)a-(uint32_t)b;break;case 0x6c:v=(uint32_t)a*(uint32_t)b;break;case 0x6d:if((uint32_t)b==0)goto trap;if((int32_t)a==INT32_MIN&&(int32_t)b==-1)goto trap;v=(uint32_t)((int32_t)a/(int32_t)b);break;case 0x6e:if((uint32_t)b==0)goto trap;v=(uint32_t)a/(uint32_t)b;break;case 0x6f:if((uint32_t)b==0)goto trap;v=((int32_t)a==INT32_MIN&&(int32_t)b==-1)?0u:(uint32_t)((int32_t)a%(int32_t)b);break;case 0x70:if((uint32_t)b==0)goto trap;v=(uint32_t)a%(uint32_t)b;break;case 0x71:v=(uint32_t)a&(uint32_t)b;break;case 0x72:v=(uint32_t)a|(uint32_t)b;break;case 0x73:v=(uint32_t)a^(uint32_t)b;break;case 0x74:v=(uint32_t)a<<((uint32_t)b&31);break;case 0x75:v=(uint32_t)((int32_t)a>>((uint32_t)b&31));break;case 0x76:v=(uint32_t)a>>((uint32_t)b&31);break;case 0x77:v=((uint32_t)a<<((uint32_t)b&31))|((uint32_t)a>>((32-(uint32_t)b)&31));break;default:v=((uint32_t)a>>((uint32_t)b&31))|((uint32_t)a<<((32-(uint32_t)b)&31));break;}if(!push(st,&sp,v))goto stackerr;break;
        default:goto unsupported;}
        if(c.bad)goto trap;
    }
    if(!done){snprintf(i->error,sizeof i->error,"function body ended without end opcode");free(locals);return 0;}if(sp<(int)t->nresults){free(locals);snprintf(i->error,sizeof i->error,"function did not return enough values");return 0;}for(uint32_t k=t->nresults;k>0;--k){if(!pop(st,&sp,&out[k-1].bits)){free(locals);snprintf(i->error,sizeof i->error,"function did not return a value");return 0;}out[k-1].type=t->result_types[k-1];}if(out_count)*out_count=t->nresults;free(locals);return 1;
stackerr:snprintf(i->error,sizeof i->error,"invalid operand stack or local index");free(locals);return 0;
trap:snprintf(i->error,sizeof i->error,"WebAssembly trap (bounds, division, or invalid immediate)");free(locals);return 0;
unsupported:snprintf(i->error,sizeof i->error,"instruction is outside the native runtime subset");free(locals);return 0;}
int mlb_wasm_rt_call_multi(MlbWasmInstance*i,uint32_t fi,const MlbWasmValue*a,size_t n,MlbWasmValue*results,size_t results_capacity,size_t*results_count,char*e,size_t ec){if(results_count)*results_count=0;if(!i)return errf(e,ec,"Invalid WebAssembly instance");if(fi>=i->module->nfuncs)return errf(e,ec,"call target out of range");WType*t=&i->module->types[i->module->funcs[fi].type];if(t->nresults>results_capacity||(t->nresults&&!results))return errf(e,ec,"WebAssembly result buffer is too small");i->error[0]=0;uint64_t fuel=WASM_FUEL;size_t got=0;int ok=invoke_multi(i,fi,a,n,results,results_capacity,&got,0,&fuel);if(!ok){if(e&&ec)snprintf(e,ec,"%s",i->error[0]?i->error:"WebAssembly runtime trap");return 0;}if(results_count)*results_count=got;if(e&&ec)e[0]=0;return 1;}
int mlb_wasm_rt_call(MlbWasmInstance*i,uint32_t fi,const MlbWasmValue*a,size_t n,MlbWasmValue*r,int*has,char*e,size_t ec){if(!i)return errf(e,ec,"Invalid WebAssembly instance");if(fi>=i->module->nfuncs)return errf(e,ec,"call target out of range");WType*t=&i->module->types[i->module->funcs[fi].type];if(t->nresults>1)return errf(e,ec,"function has multiple results; use mlb_wasm_rt_call_multi");MlbWasmValue one={0,0};size_t count=0;if(!mlb_wasm_rt_call_multi(i,fi,a,n,t->nresults?&one:NULL,t->nresults,&count,e,ec))return 0;if(r&&count)*r=one;if(has)*has=count!=0;return 1;}
uint8_t *mlb_wasm_rt_memory_data(MlbWasmInstance*i,size_t*n,uint32_t*p,uint64_t*epoch){if(!i||!i->has_memory)return NULL;return mlb_wasm_rt_memory_resource_data(i->memory,n,p,epoch);}
int64_t mlb_wasm_rt_memory_grow(MlbWasmInstance*i,uint32_t d){if(!i||!i->has_memory)return -1;return mlb_wasm_rt_memory_resource_grow(i->memory,d);}
size_t mlb_wasm_rt_custom_section_count(const MlbWasmModule*m,const char*name){if(!m||!name)return 0;size_t n=0;for(uint32_t i=0;i<m->ncustom;i++)if(!strcmp(name,m->custom[i].name))n++;return n;}
int mlb_wasm_rt_custom_section_copy(const MlbWasmModule*m,const char*name,size_t index,uint8_t**bytes,size_t*len){if(!m||!name||!bytes||!len)return 0;for(uint32_t i=0;i<m->ncustom;i++)if(!strcmp(name,m->custom[i].name)){if(index--==0){uint8_t*p=malloc(m->custom[i].length?m->custom[i].length:1);if(!p)return 0;memcpy(p,m->custom[i].bytes,m->custom[i].length);*bytes=p;*len=m->custom[i].length;return 1;}}return 0;}
const char*mlb_wasm_rt_last_error(const MlbWasmInstance*i){return i?i->error:"Invalid instance";}
