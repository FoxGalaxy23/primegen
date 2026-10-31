#include "shaping.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <math.h>

/* This helper is intentionally independent of ICU/HarfBuzz so the browser's
 * fallback renderer can use exactly the same caret-boundary policy. */
static size_t hit_utf8_next(const unsigned char *s, size_t len, size_t i) {
    unsigned char c = s[i];
    size_t step = c < 0x80 ? 1 : (c >= 0xC2 && c <= 0xDF) ? 2 :
                  (c >= 0xE0 && c <= 0xEF) ? 3 :
                  (c >= 0xF0 && c <= 0xF4) ? 4 : 1;
    if (step > len - i) return i + 1;
    for (size_t k = 1; k < step; ++k)
        if ((s[i + k] & 0xC0u) != 0x80u) return i + 1;
    /* Reject overlong/surrogate/out-of-range sequences as single-byte input. */
    if (step == 3 && c == 0xE0 && s[i + 1] < 0xA0) return i + 1;
    if (step == 3 && c == 0xED && s[i + 1] >= 0xA0) return i + 1;
    if (step == 4 && c == 0xF0 && s[i + 1] < 0x90) return i + 1;
    if (step == 4 && c == 0xF4 && s[i + 1] >= 0x90) return i + 1;
    return i + step;
}

int ttf_hit_test_byte_offset(const char *utf8, size_t utf8_len,
                             const int *caret_offsets, size_t offset_count,
                             float x, size_t *out_byte_offset) {
    if (!utf8 || !caret_offsets || !out_byte_offset || !isfinite(x) ||
        utf8_len == SIZE_MAX || offset_count < utf8_len + 1) return 0;
    size_t best = 0;
    float best_dist = fabsf((float)caret_offsets[0] - x);
    size_t i = 0;
    while (i < utf8_len) {
        size_t next = hit_utf8_next((const unsigned char *)utf8, utf8_len, i);
        if (next <= i || next > utf8_len) return 0;
        /* Equal positions are common inside a grapheme (base + marks/ZWJ).
         * Do not expose those byte boundaries to mouse hit-testing. */
        if (caret_offsets[next] != caret_offsets[i]) {
            float d = fabsf((float)caret_offsets[next] - x);
            if (d < best_dist || (d == best_dist && next < best)) {
                best_dist = d;
                best = next;
            }
        }
        i = next;
    }
    /* Empty strings and zero-advance trailing clusters still need a stable end. */
    if (utf8_len && caret_offsets[utf8_len] != caret_offsets[0]) {
        float d = fabsf((float)caret_offsets[utf8_len] - x);
        if (d < best_dist) best = utf8_len;
    }
    *out_byte_offset = best;
    return 1;
}

int ttf_visual_caret_step(const TTFVisualCaretStop *stops, size_t count,
                          size_t current, int direction, float preferred_x,
                          size_t *out_next) {
    if (!stops || !count || current >= count || !out_next ||
        (direction != -1 && direction != 1 && direction != -2 && direction != 2) ||
        !isfinite(preferred_x)) return 0;
    const TTFVisualCaretStop *cur = &stops[current];
    size_t best = current;
    if (direction == -1 || direction == 1) {
        float best_dx = INFINITY;
        for (size_t i = 0; i < count; ++i) {
            if (stops[i].row != cur->row) continue;
            float dx = stops[i].x - cur->x;
            if (direction < 0 && dx < -0.5f && -dx < best_dx) { best = i; best_dx = -dx; }
            if (direction > 0 && dx > 0.5f && dx < best_dx) { best = i; best_dx = dx; }
        }
        if (best == current) {
            if (direction < 0 && current > 0) best = current - 1;
            else if (direction > 0 && current + 1 < count) best = current + 1;
        }
    } else {
        int wanted_row = -1;
        for (size_t i = 0; i < count; ++i) {
            if (direction < 0 && stops[i].row >= cur->row) continue;
            if (direction > 0 && stops[i].row <= cur->row) continue;
            if (wanted_row < 0 || (direction < 0 && stops[i].row > wanted_row) ||
                (direction > 0 && stops[i].row < wanted_row)) wanted_row = stops[i].row;
        }
        if (wanted_row < 0) return 0;
        float best_dx = INFINITY;
        for (size_t i = 0; i < count; ++i) if (stops[i].row == wanted_row) {
            float dx = fabsf(stops[i].x - preferred_x);
            if (dx < best_dx) { best = i; best_dx = dx; }
        }
        if (best == current) return 0;
    }
    if (best == current) return 0;
    *out_next = best;
    return 1;
}

/* Unicode word classification is shared by keyboard navigation and tests.
 * ICU provides complete general categories when available; the fallback keeps
 * ASCII exact and recognises common non-ASCII writing-system ranges. */
#if !defined(_WIN32) && defined(TTF_HAVE_ICU)
#include <unicode/uchar.h>
int ttf_unicode_is_word_codepoint(uint32_t cp) {
    int8_t t = u_charType((UChar32)cp);
    switch (t) {
        case U_UPPERCASE_LETTER: case U_LOWERCASE_LETTER: case U_TITLECASE_LETTER:
        case U_MODIFIER_LETTER: case U_OTHER_LETTER:
        case U_NON_SPACING_MARK: case U_ENCLOSING_MARK: case U_COMBINING_SPACING_MARK:
        case U_DECIMAL_DIGIT_NUMBER: case U_LETTER_NUMBER: case U_OTHER_NUMBER:
        case U_CONNECTOR_PUNCTUATION:
            return 1;
        default:
            return 0;
    }
}
#else
int ttf_unicode_is_word_codepoint(uint32_t cp) {
    if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') ||
        (cp >= '0' && cp <= '9') || cp == '_') return 1;
    /* Combining marks and common scripts (Latin extensions, Greek, Cyrillic,
       Hebrew, Arabic, Indic, Southeast Asian, CJK) are word characters. */
    if ((cp >= 0x00C0 && cp <= 0x02FF) || (cp >= 0x0300 && cp <= 0x036F) ||
        (cp >= 0x0370 && cp <= 0x052F) || (cp >= 0x0590 && cp <= 0x08FF) ||
        (cp >= 0x0900 && cp <= 0x1CFF) || (cp >= 0x1E00 && cp <= 0x1EFF) ||
        (cp >= 0x2C00 && cp <= 0xA7FF) || (cp >= 0xA800 && cp <= 0xABFF) ||
        (cp >= 0xAC00 && cp <= 0xD7AF) || (cp >= 0xF900 && cp <= 0xFAFF) ||
        (cp >= 0xFF10 && cp <= 0xFF19) || (cp >= 0x10400 && cp <= 0x1EFFF)) {
        /* Exclude common Unicode punctuation and symbols from broad ranges. */
        if ((cp >= 0x2000 && cp <= 0x206F) || (cp >= 0x2190 && cp <= 0x2BFF) ||
            (cp >= 0x1F000 && cp <= 0x1FAFF)) return 0;
        return 1;
    }
    return 0;
}
#endif

static int word_utf8_decode_at(const unsigned char *s, size_t len, size_t at,
                               uint32_t *out_cp, size_t *out_next) {
    if (!s || at >= len || !out_cp || !out_next) return 0;
    unsigned char c = s[at]; uint32_t cp; size_t n;
    if (c < 0x80) { cp = c; n = 1; }
    else if (c >= 0xC2 && c <= 0xDF) { cp = c & 0x1Fu; n = 2; }
    else if (c >= 0xE0 && c <= 0xEF) { cp = c & 0x0Fu; n = 3; }
    else if (c >= 0xF0 && c <= 0xF4) { cp = c & 7u; n = 4; }
    else { *out_cp = c; *out_next = at + 1; return 1; }
    if (n > len - at) { *out_cp = c; *out_next = at + 1; return 1; }
    for (size_t i = 1; i < n; ++i) {
        if ((s[at+i] & 0xC0u) != 0x80u) { *out_cp = c; *out_next = at + 1; return 1; }
        cp = (cp << 6) | (s[at+i] & 0x3Fu);
    }
    if ((n == 3 && ((c == 0xE0 && s[at+1] < 0xA0) || (c == 0xED && s[at+1] >= 0xA0))) ||
        (n == 4 && ((c == 0xF0 && s[at+1] < 0x90) || (c == 0xF4 && s[at+1] >= 0x90)))) {
        *out_cp = c; *out_next = at + 1; return 1;
    }
    *out_cp = cp; *out_next = at + n; return 1;
}
static size_t word_utf8_prev(const unsigned char *s, size_t offset) {
    size_t at = offset ? offset - 1 : 0;
    while (at > 0 && (s[at] & 0xC0u) == 0x80u) --at;
    return at;
}
int ttf_text_word_step(const char *utf8, size_t len, size_t offset,
                       int direction, size_t *out_offset) {
    if (!utf8 || !out_offset || (direction != -1 && direction != 1) || offset > len) return 0;
    const unsigned char *s = (const unsigned char *)utf8;
    /* Never start at a UTF-8 continuation byte. */
    while (offset > 0 && offset < len && (s[offset] & 0xC0u) == 0x80u) --offset;
    size_t target = offset;
    if (direction < 0) {
        /* First skip separators immediately to the left, then the previous word. */
        while (target > 0) {
            size_t prev = word_utf8_prev(s, target); uint32_t cp; size_t next;
            if (!word_utf8_decode_at(s, len, prev, &cp, &next)) break;
            if (ttf_unicode_is_word_codepoint(cp)) break;
            target = prev;
        }
        while (target > 0) {
            size_t prev = word_utf8_prev(s, target); uint32_t cp; size_t next;
            if (!word_utf8_decode_at(s, len, prev, &cp, &next) || !ttf_unicode_is_word_codepoint(cp)) break;
            target = prev;
        }
    } else {
        /* Skip the word under the caret, then separators, stopping at next word start. */
        while (target < len) {
            uint32_t cp; size_t next;
            if (!word_utf8_decode_at(s, len, target, &cp, &next) || !ttf_unicode_is_word_codepoint(cp)) break;
            target = next;
        }
        while (target < len) {
            uint32_t cp; size_t next;
            if (!word_utf8_decode_at(s, len, target, &cp, &next) || ttf_unicode_is_word_codepoint(cp)) break;
            target = next;
        }
    }
    if (target == offset) return 0;
    *out_offset = target;
    return 1;
}

static int logical_prev_cp(const TTFLogicalCaretPosition *positions, size_t count,
                           size_t i, uint32_t *out_cp) {
    if (!positions || i >= count || !out_cp || !positions[i].utf8) return 0;
    const TTFLogicalCaretPosition *p = &positions[i];
    size_t len = strlen(p->utf8);
    if (p->byte_offset > len) return 0;
    if (p->byte_offset > 0) {
        size_t at = word_utf8_prev((const unsigned char *)p->utf8, p->byte_offset), next = 0;
        return word_utf8_decode_at((const unsigned char *)p->utf8, len, at, out_cp, &next);
    }
    if (i == 0 || positions[i-1].paragraph_id != p->paragraph_id || !positions[i-1].utf8) return 0;
    const TTFLogicalCaretPosition *prev = &positions[i-1];
    size_t prev_len = strlen(prev->utf8);
    if (!prev->byte_offset || prev->byte_offset > prev_len) return 0;
    size_t at = word_utf8_prev((const unsigned char *)prev->utf8, prev->byte_offset), next = 0;
    return word_utf8_decode_at((const unsigned char *)prev->utf8, prev_len, at, out_cp, &next);
}
static int logical_next_cp(const TTFLogicalCaretPosition *positions, size_t count,
                           size_t i, uint32_t *out_cp) {
    if (!positions || i >= count || !out_cp || !positions[i].utf8) return 0;
    const TTFLogicalCaretPosition *p = &positions[i];
    size_t len = strlen(p->utf8), next = 0;
    if (p->byte_offset > len) return 0;
    if (p->byte_offset < len)
        return word_utf8_decode_at((const unsigned char *)p->utf8, len, p->byte_offset, out_cp, &next);
    if (i + 1 >= count || positions[i+1].paragraph_id != p->paragraph_id || !positions[i+1].utf8) return 0;
    const TTFLogicalCaretPosition *after = &positions[i+1];
    size_t after_len = strlen(after->utf8);
    if (after->byte_offset >= after_len) return 0;
    return word_utf8_decode_at((const unsigned char *)after->utf8, after_len, after->byte_offset, out_cp, &next);
}
int ttf_logical_caret_move_word(const TTFLogicalCaretPosition *positions, size_t count,
                                size_t current, int direction, size_t *out_target) {
    if (!positions || !count || current >= count || !out_target ||
        (direction != -1 && direction != 1)) return 0;
    size_t i = current; uint32_t cp = 0;
    if (direction < 0) {
        while (i > 0 && logical_prev_cp(positions,count,i,&cp) && !ttf_unicode_is_word_codepoint(cp)) --i;
        while (i > 0 && logical_prev_cp(positions,count,i,&cp) && ttf_unicode_is_word_codepoint(cp)) --i;
    } else {
        while (i + 1 < count && logical_next_cp(positions,count,i,&cp) && ttf_unicode_is_word_codepoint(cp)) ++i;
        while (i + 1 < count && logical_next_cp(positions,count,i,&cp) && !ttf_unicode_is_word_codepoint(cp)) ++i;
    }
    if (i == current) return 0;
    *out_target = i; return 1;
}
int ttf_logical_caret_move_paragraph(const TTFLogicalCaretPosition *positions, size_t count,
                                     size_t current, int direction, size_t *out_target) {
    if (!positions || !count || current >= count || !out_target ||
        (direction != -1 && direction != 1)) return 0;
    int pid = positions[current].paragraph_id;
    size_t first = current, last = current;
    while (first > 0 && positions[first-1].paragraph_id == pid) --first;
    while (last + 1 < count && positions[last+1].paragraph_id == pid) ++last;
    if (direction < 0) {
        if (current > first) { *out_target = first; return 1; }
        if (!first) return 0;
        size_t prev = first - 1, prev_first = prev;
        int prev_pid = positions[prev].paragraph_id;
        while (prev_first > 0 && positions[prev_first-1].paragraph_id == prev_pid) --prev_first;
        *out_target = prev_first; return 1;
    }
    if (current < last) { *out_target = last; return 1; }
    if (last + 1 >= count) return 0;
    size_t next = last + 1, next_last = next;
    int next_pid = positions[next].paragraph_id;
    while (next_last + 1 < count && positions[next_last+1].paragraph_id == next_pid) ++next_last;
    *out_target = next_last; return 1;
}

#if !defined(_WIN32) && defined(TTF_HAVE_ICU)
#include <dlfcn.h>
#include <unicode/ubidi.h>
#include <unicode/ustring.h>
#include <unicode/utf8.h>
#include <unicode/utf16.h>
#include <unicode/uchar.h>

typedef struct hb_blob_t hb_blob_t; typedef struct hb_face_t hb_face_t;
typedef struct hb_font_t hb_font_t; typedef struct hb_buffer_t hb_buffer_t;
typedef uint32_t hb_tag_t; typedef int hb_bool_t; typedef int hb_direction_t;
typedef struct { uint32_t codepoint, mask, cluster, var1, var2; } hb_glyph_info_t;
typedef struct { int32_t x_advance, y_advance, x_offset, y_offset; uint32_t var; } hb_glyph_position_t;
typedef hb_blob_t *(*p_blob_create)(const char*, unsigned, int, void*, void(*)(void*));
typedef void (*p_blob_destroy)(hb_blob_t*); typedef hb_face_t *(*p_face_create)(hb_blob_t*, unsigned);
typedef void (*p_face_destroy)(hb_face_t*); typedef unsigned (*p_face_upem)(hb_face_t*);
typedef hb_font_t *(*p_font_create)(hb_face_t*); typedef void (*p_font_destroy)(hb_font_t*);
typedef void (*p_ot_funcs)(hb_font_t*); typedef void (*p_font_scale)(hb_font_t*, int, int);
typedef hb_buffer_t *(*p_buffer_create)(void); typedef void (*p_buffer_destroy)(hb_buffer_t*);
typedef void (*p_buffer_add_utf8)(hb_buffer_t*, const char*, int, unsigned, int);
typedef void (*p_buffer_guess)(hb_buffer_t*); typedef void (*p_shape)(hb_font_t*, hb_buffer_t*, const void*, unsigned);
typedef hb_glyph_info_t *(*p_infos)(hb_buffer_t*, unsigned*);
typedef hb_glyph_position_t *(*p_positions)(hb_buffer_t*, unsigned*);
typedef void (*p_buffer_direction)(hb_buffer_t*, hb_direction_t);
typedef struct {
 void *handle; p_blob_create blob_create; p_blob_destroy blob_destroy; p_face_create face_create;
 p_face_destroy face_destroy; p_face_upem face_upem; p_font_create font_create; p_font_destroy font_destroy;
 p_ot_funcs ot_funcs; p_font_scale font_scale; p_buffer_create buffer_create; p_buffer_destroy buffer_destroy;
 p_buffer_add_utf8 buffer_add_utf8; p_buffer_guess buffer_guess; p_shape shape; p_infos infos;
 p_positions positions; p_buffer_direction buffer_direction; int tried;
} HB;
static HB g_hb;
#define SYM(field, name) do { *(void **)(&g_hb.field) = dlsym(g_hb.handle, name); if (!g_hb.field) goto fail; } while (0)
static int hb_init(void) {
 if (g_hb.tried) return g_hb.handle != NULL && g_hb.shape != NULL;
 g_hb.tried = 1;
 g_hb.handle = dlopen("libharfbuzz.so.0", RTLD_LAZY|RTLD_LOCAL);
 if (!g_hb.handle) g_hb.handle = dlopen("libharfbuzz.so", RTLD_LAZY|RTLD_LOCAL);
 if (!g_hb.handle) return 0;
 SYM(blob_create,"hb_blob_create"); SYM(blob_destroy,"hb_blob_destroy"); SYM(face_create,"hb_face_create");
 SYM(face_destroy,"hb_face_destroy"); SYM(face_upem,"hb_face_get_upem"); SYM(font_create,"hb_font_create");
 SYM(font_destroy,"hb_font_destroy"); SYM(ot_funcs,"hb_ot_font_set_funcs"); SYM(font_scale,"hb_font_set_scale");
 SYM(buffer_create,"hb_buffer_create"); SYM(buffer_destroy,"hb_buffer_destroy"); SYM(buffer_add_utf8,"hb_buffer_add_utf8");
 SYM(buffer_guess,"hb_buffer_guess_segment_properties"); SYM(shape,"hb_shape"); SYM(infos,"hb_buffer_get_glyph_infos");
 SYM(positions,"hb_buffer_get_glyph_positions"); SYM(buffer_direction,"hb_buffer_set_direction");
 return 1;
fail: dlclose(g_hb.handle); memset(&g_hb,0,sizeof g_hb); g_hb.tried=1; return 0;
}
static int append(TTFShapedGlyph **v,size_t *n,size_t *cap,const hb_glyph_info_t *gi,
                  const hb_glyph_position_t *gp,unsigned count,size_t byte_base,uintptr_t token,int rtl) {
 if ((size_t)count > SIZE_MAX-*n) return 0;
 size_t need=*n+(size_t)count;
 if (need > *cap) { size_t nc=*cap?*cap:32; while(nc<need) { if(nc>SIZE_MAX/2/sizeof(**v)) return 0; nc*=2; }
   TTFShapedGlyph *nv=realloc(*v,nc*sizeof(**v)); if(!nv) return 0; *v=nv; *cap=nc; }
 for(unsigned i=0;i<count;i++) { TTFShapedGlyph *o=&(*v)[(*n)++]; o->glyph=gi[i].codepoint; o->cluster=(uint32_t)(byte_base+gi[i].cluster); o->x_advance=gp[i].x_advance; o->y_advance=gp[i].y_advance; o->x_offset=gp[i].x_offset; o->y_offset=gp[i].y_offset; o->font_token=token; o->rtl=(uint8_t)(rtl!=0); }
 return 1;
}
static int format_or_mark(uint32_t cp) {
 int8_t t=u_charType((UChar32)cp);
 return t==U_NON_SPACING_MARK || t==U_COMBINING_SPACING_MARK || t==U_ENCLOSING_MARK ||
        t==U_FORMAT_CHAR || (cp>=0xFE00 && cp<=0xFE0F) || (cp>=0xE0100 && cp<=0xE01EF);
}
static int is_gcb_control(int v) { return v==U_GCB_CONTROL || v==U_GCB_CR || v==U_GCB_LF; }
static int is_gcb_extend(int v) { return v==U_GCB_EXTEND || v==U_GCB_E_MODIFIER; }

/* UAX #29 extended grapheme boundary rules. Keep this small implementation
 * next to shaping so font fallback never splits a grapheme across typefaces. */
static int grapheme_break_before(const uint32_t *cp, size_t i, size_t ri_run_before) {
 if (!i) return 1;
 int a=u_getIntPropertyValue((UChar32)cp[i-1],UCHAR_GRAPHEME_CLUSTER_BREAK);
 int b=u_getIntPropertyValue((UChar32)cp[i],UCHAR_GRAPHEME_CLUSTER_BREAK);
 /* GB3, GB4, GB5 */
 if (a==U_GCB_CR && b==U_GCB_LF) return 0;
 if (is_gcb_control(a) || is_gcb_control(b)) return 1;
 /* GB6-GB8: Hangul syllable composition. */
 if (a==U_GCB_L && (b==U_GCB_L || b==U_GCB_V || b==U_GCB_LV || b==U_GCB_LVT)) return 0;
 if ((a==U_GCB_LV || a==U_GCB_V) && (b==U_GCB_V || b==U_GCB_T)) return 0;
 if ((a==U_GCB_LVT || a==U_GCB_T) && b==U_GCB_T) return 0;
 /* GB9, GB9a, GB9b. */
 if (is_gcb_extend(b) || b==U_GCB_ZWJ || b==U_GCB_SPACING_MARK) return 0;
 if (a==U_GCB_PREPEND) return 0;
 /* GB9c: Indic conjuncts (available from ICU 76). */
#if U_ICU_VERSION_MAJOR_NUM >= 76
 if (u_getIntPropertyValue((UChar32)cp[i],UCHAR_INDIC_CONJUNCT_BREAK)==U_INCB_CONSONANT) {
   size_t j=i; int saw_linker=0;
   while(j>0) {
     int incb=u_getIntPropertyValue((UChar32)cp[j-1],UCHAR_INDIC_CONJUNCT_BREAK);
     if(incb==U_INCB_LINKER) { saw_linker=1; j--; continue; }
     if(incb==U_INCB_EXTEND) { j--; continue; }
     break;
   }
   if(saw_linker && j>0 && u_getIntPropertyValue((UChar32)cp[j-1],UCHAR_INDIC_CONJUNCT_BREAK)==U_INCB_CONSONANT) return 0;
 }
#endif
 /* GB11: Extended_Pictographic Extend* ZWJ x Extended_Pictographic. */
 if (u_hasBinaryProperty((UChar32)cp[i],UCHAR_EXTENDED_PICTOGRAPHIC) && a==U_GCB_ZWJ) {
   size_t j=i-1;
   while(j>0 && is_gcb_extend(u_getIntPropertyValue((UChar32)cp[j-1],UCHAR_GRAPHEME_CLUSTER_BREAK))) j--;
   if(j>0 && u_hasBinaryProperty((UChar32)cp[j-1],UCHAR_EXTENDED_PICTOGRAPHIC)) return 0;
 }
 /* GB10 compatibility for older emoji property data. */
 if (b==U_GCB_E_MODIFIER) {
   size_t j=i;
   while(j>0 && is_gcb_extend(u_getIntPropertyValue((UChar32)cp[j-1],UCHAR_GRAPHEME_CLUSTER_BREAK))) j--;
   if(j>0) { int x=u_getIntPropertyValue((UChar32)cp[j-1],UCHAR_GRAPHEME_CLUSTER_BREAK); if(x==U_GCB_E_BASE||x==U_GCB_E_BASE_GAZ) return 0; }
 }
 /* GB12/GB13: pair regional indicators, from the start of the RI run. */
 if(a==U_GCB_REGIONAL_INDICATOR && b==U_GCB_REGIONAL_INDICATOR) {
   if(ri_run_before&1u) return 0;
 }
 return 1; /* GB999 */
}
static UCharDirection U_CALLCONV force_bidi_class(const void *context, UChar32 cp) {
 /* Match the core behavior of an LRO/RLO around an inline run: ordinary
  * characters, whitespace and neutrals get the forced direction, while
  * paragraph separators and explicit bidi formatting controls keep their
  * semantics. This also avoids passing ICU's override bit as a paragraph
  * level, which is not a valid ubidi_setPara() base level. */
 int rtl = context ? *(const int *)context : 0;
 if (cp == 0x000A || cp == 0x000D || cp == 0x001C || cp == 0x001D || cp == 0x001E || cp == 0x0085 || cp == 0x2028 || cp == 0x2029)
   return U_BLOCK_SEPARATOR;
 if ((cp >= 0x202A && cp <= 0x202E) || (cp >= 0x2066 && cp <= 0x2069))
   return (UCharDirection)(u_getIntPropertyMaxValue(UCHAR_BIDI_CLASS) + 1);
 return rtl ? U_RIGHT_TO_LEFT : U_LEFT_TO_RIGHT;
}
static int face_is_valid(const TTFShapeFace *face) {
 return face && face->data && face->data_len && face->data_len<=UINT_MAX && face->upem>0;
}
static int shape_span(const TTFShapeFace *face,const char *txt,size_t len,int rtl,size_t byte_base,
                      TTFShapedGlyph **all,size_t *n,size_t *cap) {
 if(!face_is_valid(face)||!len||len>INT_MAX) return 0;
 hb_blob_t *blob=g_hb.blob_create((const char*)face->data,(unsigned)face->data_len,1,NULL,NULL); if(!blob) return 0;
 hb_face_t *hface=g_hb.face_create(blob,0); if(!hface) {g_hb.blob_destroy(blob);return 0;}
 hb_font_t *font=g_hb.font_create(hface); if(!font) {g_hb.face_destroy(hface);g_hb.blob_destroy(blob);return 0;}
 g_hb.ot_funcs(font); g_hb.font_scale(font,face->upem,face->upem);
 hb_buffer_t *buf=g_hb.buffer_create(); if(!buf) {g_hb.font_destroy(font);g_hb.face_destroy(hface);g_hb.blob_destroy(blob);return 0;}
 g_hb.buffer_add_utf8(buf,txt,(int)len,0,(int)len);
 /* HarfBuzz enum: 4=LTR and 5=RTL. */
 g_hb.buffer_direction(buf,rtl?5:4); g_hb.buffer_guess(buf); g_hb.shape(font,buf,NULL,0);
 unsigned ni=0,np=0; hb_glyph_info_t *gi=g_hb.infos(buf,&ni); hb_glyph_position_t *gp=g_hb.positions(buf,&np);
 int ok=gi&&gp&&ni==np&&append(all,n,cap,gi,gp,ni,byte_base,face->token,rtl);
 g_hb.buffer_destroy(buf); g_hb.font_destroy(font); g_hb.face_destroy(hface); g_hb.blob_destroy(blob);
 return ok;
}
static int utf16_to_utf8(const UChar *u,int32_t start,int32_t count,char **out,int32_t *out_len) {
 UErrorCode ec=U_ZERO_ERROR; int32_t need=0;
 u_strToUTF8(NULL,0,&need,u+start,count,&ec);
 if(ec!=U_BUFFER_OVERFLOW_ERROR && U_FAILURE(ec)) return 0;
 char *s=malloc((size_t)need+1); if(!s) return 0;
 ec=U_ZERO_ERROR; u_strToUTF8(s,need+1,NULL,u+start,count,&ec);
 if(U_FAILURE(ec)) {free(s);return 0;} *out=s; *out_len=need; return 1;
}

static int bidi_scope_controls(uint8_t mode,UChar open[2],int *open_n,UChar close[2],int *close_n) {
 *open_n=0; *close_n=0;
 switch(mode) {
  case TTF_BIDI_SEGMENT_EMBED_LTR: open[(*open_n)++]=0x202A;close[(*close_n)++]=0x202C;break;
  case TTF_BIDI_SEGMENT_EMBED_RTL: open[(*open_n)++]=0x202B;close[(*close_n)++]=0x202C;break;
  case TTF_BIDI_SEGMENT_OVERRIDE_LTR: open[(*open_n)++]=0x202D;close[(*close_n)++]=0x202C;break;
  case TTF_BIDI_SEGMENT_OVERRIDE_RTL: open[(*open_n)++]=0x202E;close[(*close_n)++]=0x202C;break;
  case TTF_BIDI_SEGMENT_ISOLATE_LTR: open[(*open_n)++]=0x2066;close[(*close_n)++]=0x2069;break;
  case TTF_BIDI_SEGMENT_ISOLATE_RTL: open[(*open_n)++]=0x2067;close[(*close_n)++]=0x2069;break;
  case TTF_BIDI_SEGMENT_ISOLATE_FIRST_STRONG: open[(*open_n)++]=0x2068;close[(*close_n)++]=0x2069;break;
  case TTF_BIDI_SEGMENT_ISOLATE_OVERRIDE_LTR:
   open[(*open_n)++]=0x2066;open[(*open_n)++]=0x202D;close[(*close_n)++]=0x202C;close[(*close_n)++]=0x2069;break;
  case TTF_BIDI_SEGMENT_ISOLATE_OVERRIDE_RTL:
   open[(*open_n)++]=0x2067;open[(*open_n)++]=0x202E;close[(*close_n)++]=0x202C;close[(*close_n)++]=0x2069;break;
  default: break;
 }
 return *open_n+*close_n;
}
static unsigned bidi_scope_lcp(const TTFBidiScope *a,const TTFBidiScope *b) {
 unsigned n=a->count<b->count?a->count:b->count, i=0;
 while(i<n && a->ids[i]==b->ids[i] && a->modes[i]==b->modes[i]) i++;
 return i;
}
static int bidi_append_scope_open(UChar *u,int32_t *pos,uint8_t mode) {
 UChar op[2],cl[2];int on=0,cn=0;
 if(!bidi_scope_controls(mode,op,&on,cl,&cn)||on!=cn)return 0;
 for(int i=0;i<on;i++)u[(*pos)++]=op[i];
 return on;
}
static int bidi_append_scope_close(UChar *u,int32_t *pos,uint8_t mode) {
 UChar op[2],cl[2];int on=0,cn=0;
 if(!bidi_scope_controls(mode,op,&on,cl,&cn)||on!=cn)return 0;
 for(int i=0;i<cn;i++)u[(*pos)++]=cl[i];
 return cn;
}
int ttf_bidi_reorder_segments_scoped(const char *const *segments,const size_t *lengths,
                                     const TTFBidiScope *scopes,size_t count,
                                     int base_direction,size_t *visual_to_logical) {
 if(!segments||!lengths||!visual_to_logical||!count||count>INT32_MAX ||
    count>SIZE_MAX/sizeof(int32_t) || count>SIZE_MAX/sizeof(UBiDiLevel) ||
    count>SIZE_MAX/sizeof(size_t)) return 0;
 size_t total_bytes=0;
 int32_t total_u16=0;
 int32_t *starts=calloc(count,sizeof(*starts));
 int32_t *ends=calloc(count,sizeof(*ends));
 int32_t *src_units=calloc(count,sizeof(*src_units));
 UBiDiLevel *levels=calloc(count,sizeof(*levels));
 int32_t *map=calloc(count,sizeof(*map));
 if(!starts||!ends||!src_units||!levels||!map) {free(starts);free(ends);free(src_units);free(levels);free(map);return 0;}
 TTFBidiScope empty;memset(&empty,0,sizeof empty);
 for(size_t i=0;i<count;i++) {
   if(!segments[i] || lengths[i]>(size_t)INT32_MAX || total_bytes>SIZE_MAX-lengths[i] ||
      total_bytes+lengths[i]>(size_t)INT32_MAX) {free(starts);free(ends);free(src_units);free(levels);free(map);return 0;}
   total_bytes+=lengths[i];
   UErrorCode ec=U_ZERO_ERROR; int32_t units=0;
   u_strFromUTF8(NULL,0,&units,segments[i],(int32_t)lengths[i],&ec);
   if(ec!=U_BUFFER_OVERFLOW_ERROR && U_FAILURE(ec)) {free(starts);free(ends);free(src_units);free(levels);free(map);return 0;}
   if(units<0 || total_u16>INT32_MAX-units) {free(starts);free(ends);free(src_units);free(levels);free(map);return 0;}
   src_units[i]=units; total_u16+=units;
   if(scopes && scopes[i].count>TTF_BIDI_MAX_SCOPE_DEPTH) {free(starts);free(ends);free(src_units);free(levels);free(map);return 0;}
   const TTFBidiScope *prev=i?(scopes?&scopes[i-1]:&empty):&empty;
   const TTFBidiScope *cur=scopes?&scopes[i]:&empty;
   unsigned common=bidi_scope_lcp(prev,cur);
   int wrappers=0;
   for(unsigned k=prev->count;k>common;k--) {
     UChar op[2],cl[2];int on=0,cn=0;
     if(!bidi_scope_controls(prev->modes[k-1],op,&on,cl,&cn)||on!=cn || wrappers>INT32_MAX-cn) {free(starts);free(ends);free(src_units);free(levels);free(map);return 0;}
     wrappers+=cn;
   }
   for(unsigned k=common;k<cur->count;k++) {
     UChar op[2],cl[2];int on=0,cn=0;
     if(!bidi_scope_controls(cur->modes[k],op,&on,cl,&cn)||on!=cn || wrappers>INT32_MAX-on) {free(starts);free(ends);free(src_units);free(levels);free(map);return 0;}
     wrappers+=on;
   }
   if(total_u16>INT32_MAX-wrappers) {free(starts);free(ends);free(src_units);free(levels);free(map);return 0;}
   total_u16+=wrappers;
 }
 if(scopes) for(unsigned k=scopes[count-1].count;k>0;k--) {
   UChar op[2],cl[2];int on=0,cn=0;
   if(!bidi_scope_controls(scopes[count-1].modes[k-1],op,&on,cl,&cn)||on!=cn || total_u16>INT32_MAX-cn) {free(starts);free(ends);free(src_units);free(levels);free(map);return 0;}
   total_u16+=cn;
 }
 if(total_u16<=0) {for(size_t i=0;i<count;i++)visual_to_logical[i]=i;free(starts);free(ends);free(src_units);free(levels);free(map);return 1;}
 if ((size_t)total_u16 > SIZE_MAX/sizeof(UChar)-1) { free(starts); free(ends); free(src_units); free(levels); free(map); return 0; }
 UChar *u=malloc(((size_t)total_u16+1)*sizeof(*u));
 if(!u) {free(starts);free(ends);free(src_units);free(levels);free(map);return 0;}
 int32_t pos=0;
 for(size_t i=0;i<count;i++) {
   const TTFBidiScope *prev=i?(scopes?&scopes[i-1]:&empty):&empty;
   const TTFBidiScope *cur=scopes?&scopes[i]:&empty;
   unsigned common=bidi_scope_lcp(prev,cur);
   for(unsigned k=prev->count;k>common;k--) {
     if(!bidi_append_scope_close(u,&pos,prev->modes[k-1])) {free(u);free(starts);free(ends);free(src_units);free(levels);free(map);return 0;}
   }
   for(unsigned k=common;k<cur->count;k++) {
     if(!bidi_append_scope_open(u,&pos,cur->modes[k])) {free(u);free(starts);free(ends);free(src_units);free(levels);free(map);return 0;}
   }
   starts[i]=pos;
   UErrorCode ec=U_ZERO_ERROR; int32_t wrote=0;
   ec=U_ZERO_ERROR;
   u_strFromUTF8(u+pos,src_units[i]+1,&wrote,segments[i],(int32_t)lengths[i],&ec);
   if(U_FAILURE(ec)||wrote!=src_units[i]) {free(u);free(starts);free(ends);free(src_units);free(levels);free(map);return 0;}
   pos+=wrote; ends[i]=pos;
 }
 if(scopes) for(unsigned k=scopes[count-1].count;k>0;k--) {
   if(!bidi_append_scope_close(u,&pos,scopes[count-1].modes[k-1])) {free(u);free(starts);free(ends);free(src_units);free(levels);free(map);return 0;}
 }
 u[pos]=0;
 UErrorCode ec=U_ZERO_ERROR;
 UBiDi *bidi=ubidi_open();
 if(!bidi) {free(u);free(starts);free(ends);free(src_units);free(levels);free(map);return 0;}
 UBiDiLevel para=base_direction<0?UBIDI_DEFAULT_LTR:(base_direction?UBIDI_RTL:UBIDI_LTR);
 ec=U_ZERO_ERROR; ubidi_setPara(bidi,u,total_u16,para,NULL,&ec);
 if(U_FAILURE(ec)) {ubidi_close(bidi);free(u);free(starts);free(ends);free(src_units);free(levels);free(map);return 0;}
 for(size_t i=0;i<count;i++) {
   int32_t chosen=starts[i];
   for(int32_t q=starts[i];q<ends[i];) {
     int32_t cp_start=q; UChar32 cp; U16_NEXT(u,q,ends[i],cp);
     UCharDirection d=u_charDirection(cp);
     if(d==U_LEFT_TO_RIGHT || d==U_RIGHT_TO_LEFT || d==U_RIGHT_TO_LEFT_ARABIC) { chosen=cp_start; break; }
   }
   if(chosen>=total_u16) chosen=total_u16-1;
   levels[i]=ubidi_getLevelAt(bidi,chosen);
 }
 ubidi_reorderVisual(levels,(int32_t)count,map);
 for(size_t i=0;i<count;i++) {
   if(map[i]<0 || (size_t)map[i]>=count) {ubidi_close(bidi);free(u);free(starts);free(ends);free(src_units);free(levels);free(map);return 0;}
   visual_to_logical[i]=(size_t)map[i];
 }
 ubidi_close(bidi);free(u);free(starts);free(ends);free(src_units);free(levels);free(map);return 1;
}
int ttf_bidi_reorder_segments_ex(const char *const *segments,const size_t *lengths,
                                 const uint8_t *modes,size_t count,int base_direction,
                                 size_t *visual_to_logical) {
 if(!count || count>SIZE_MAX/sizeof(TTFBidiScope)) return 0;
 TTFBidiScope *scopes=calloc(count,sizeof(*scopes));
 if(!scopes) return 0;
 for(size_t i=0;i<count;i++) if(modes && modes[i]!=TTF_BIDI_SEGMENT_NORMAL) {
   if(modes[i]>TTF_BIDI_SEGMENT_ISOLATE_OVERRIDE_RTL) {free(scopes);return 0;}
   scopes[i].count=1;scopes[i].modes[0]=modes[i];scopes[i].ids[0]=(uintptr_t)i+1;
 }
 int ok=ttf_bidi_reorder_segments_scoped(segments,lengths,scopes,count,base_direction,visual_to_logical);
 free(scopes);return ok;
}
int ttf_bidi_reorder_segments(const char *const *segments,const size_t *lengths,
                              size_t count,int base_direction,size_t *visual_to_logical) {
 return ttf_bidi_reorder_segments_ex(segments,lengths,NULL,count,base_direction,visual_to_logical);
}
int ttf_shape_text_resolved_clusters_bidi(const char *txt,size_t len,TTFShapeResolve resolver,
                                          TTFShapeClusterResolve cluster_resolver,void *ctx,
                                          int base_direction,int bidi_override,
                                          TTFShapedGlyph **out,size_t *outn) {
 if(out) *out=NULL;
 if(outn) *outn=0;
 if(!txt||!len||len>INT32_MAX||len>UINT32_MAX||!resolver||!out||!outn||!hb_init()) return 0;
 /* Validate and convert once. Cluster boundaries are Unicode grapheme boundaries;
  * byte offsets remain mapped to the original UTF-8 input for the renderer. */
 UErrorCode ec=U_ZERO_ERROR; int32_t ulen=0;
 u_strFromUTF8(NULL,0,&ulen,txt,(int32_t)len,&ec);
 if(ec!=U_BUFFER_OVERFLOW_ERROR && U_FAILURE(ec)) return 0;
 ec=U_ZERO_ERROR; UChar *u=malloc(((size_t)ulen+1)*sizeof(UChar));
 size_t *byte_at=malloc(((size_t)ulen+1)*sizeof(size_t));
 TTFShapeFace *faces=calloc((size_t)ulen+1,sizeof(TTFShapeFace));
 size_t *cp_u16=malloc(((size_t)len+1)*sizeof(size_t));
 uint32_t *cps=malloc(((size_t)len+1)*sizeof(uint32_t));
 TTFShapeFace *preferred=calloc((size_t)len+1,sizeof(TTFShapeFace));
 if(!u||!byte_at||!faces||!cp_u16||!cps||!preferred) {free(u);free(byte_at);free(faces);free(cp_u16);free(cps);free(preferred);return 0;}
 ec=U_ZERO_ERROR; u_strFromUTF8(u,ulen+1,NULL,txt,(int32_t)len,&ec);
 if(U_FAILURE(ec)) {free(u);free(byte_at);free(faces);free(cp_u16);free(cps);free(preferred);return 0;}
 int32_t ui=0,bi=0; size_t nc=0;
 while(bi<(int32_t)len) {
   int32_t begin=bi; UChar32 cp; U8_NEXT(txt,bi,(int32_t)len,cp);
   if(cp<0||nc>(size_t)len) {free(u);free(byte_at);free(faces);free(cp_u16);free(cps);free(preferred);return 0;}
   TTFShapeFace face; memset(&face,0,sizeof face);
   if(!resolver(ctx,(uint32_t)cp,&face)||!face_is_valid(&face)) {free(u);free(byte_at);free(faces);free(cp_u16);free(cps);free(preferred);return 0;}
   cps[nc]=(uint32_t)cp; cp_u16[nc]=(size_t)ui; preferred[nc]=face; nc++;
   int units=U16_LENGTH(cp);
   if(ui+units>ulen) {free(u);free(byte_at);free(faces);free(cp_u16);free(cps);free(preferred);return 0;}
   for(int k=0;k<units;k++) byte_at[ui+k]=(size_t)begin;
   ui+=units;
 }
 byte_at[ulen]=len; if(ui!=ulen) {free(u);free(byte_at);free(faces);free(cp_u16);free(cps);free(preferred);return 0;}
 cp_u16[nc]=(size_t)ulen;
 /* Assign exactly one typeface per grapheme whenever a cluster resolver exists.
  * Legacy callers retain per-scalar resolution with inherited combining marks. */
 TTFShapeFace previous; memset(&previous,0,sizeof previous);
 for(size_t i=0;i<nc;) {
   size_t j=i+1;
   size_t ri_run_before = u_getIntPropertyValue((UChar32)cps[i],UCHAR_GRAPHEME_CLUSTER_BREAK)==U_GCB_REGIONAL_INDICATOR ? 1u : 0u;
   while(j<nc) {
     if(grapheme_break_before(cps,j,ri_run_before)) break;
     if(u_getIntPropertyValue((UChar32)cps[j],UCHAR_GRAPHEME_CLUSTER_BREAK)==U_GCB_REGIONAL_INDICATOR)
       ri_run_before++;
     else ri_run_before=0;
     j++;
   }
   if(cluster_resolver) {
     TTFShapeFace chosen; memset(&chosen,0,sizeof chosen);
     if(!cluster_resolver(ctx,cps+i,j-i,&chosen)||!face_is_valid(&chosen)) {free(u);free(byte_at);free(faces);free(cp_u16);free(cps);free(preferred);return 0;}
     for(size_t k=i;k<j;k++) {
       size_t a=cp_u16[k],b=cp_u16[k+1];
       for(size_t z=a;z<b;z++) faces[z]=chosen;
     }
     previous=chosen;
   } else {
     for(size_t k=i;k<j;k++) {
       TTFShapeFace chosen=preferred[k];
       if(!chosen.covered&&format_or_mark(cps[k])&&face_is_valid(&previous)) chosen=previous;
       size_t a=cp_u16[k],b=cp_u16[k+1];
       for(size_t z=a;z<b;z++) faces[z]=chosen;
       previous=chosen;
     }
   }
   i=j;
 }
 UBiDi *bidi=ubidi_open(); if(!bidi) {free(u);free(byte_at);free(faces);free(cp_u16);free(cps);free(preferred);return 0;}
 /* CSS direction selects the paragraph base level. Keep first-strong/default
  * behavior for the old API, but use a bidi-class callback for bidi-override:
  * ICU does not accept UBIDI_LEVEL_OVERRIDE as a paragraph base level. */
 UBiDiLevel para_level = base_direction < 0 ? UBIDI_DEFAULT_LTR : (base_direction ? UBIDI_RTL : UBIDI_LTR);
 int override_rtl = base_direction > 0;
 ec=U_ZERO_ERROR;
 if (bidi_override) ubidi_setClassCallback(bidi,force_bidi_class,&override_rtl,NULL,NULL,&ec);
 if (U_SUCCESS(ec)) ubidi_setPara(bidi,u,ulen,para_level,NULL,&ec);
 if(U_FAILURE(ec)) {ubidi_close(bidi);free(u);free(byte_at);free(faces);free(cp_u16);free(cps);free(preferred);return 0;}
 int32_t runs=ubidi_countRuns(bidi,&ec); TTFShapedGlyph *all=NULL; size_t n=0,cap=0;
 for(int32_t ri=0;ri<runs&&U_SUCCESS(ec);ri++) {
   int32_t rs=0,rl=0; UBiDiDirection dir=ubidi_getVisualRun(bidi,ri,&rs,&rl);
   if(rl<=0)continue;
   int rtl=dir==UBIDI_RTL;
   /* ICU enumerates bidi runs in visual (left-to-right) order. Within an RTL
    * run the logical font spans must therefore be visited from end to start. */
   int32_t pos=rtl?rs+rl:rs, limit=rtl?rs:rs+rl;
   while(rtl?pos>limit:pos<limit) {
     int32_t a,b; uintptr_t token;
     if(!rtl) { a=pos; token=faces[a].token; b=a+1; while(b<limit&&faces[b].token==token)b++; pos=b; }
     else { b=pos; token=faces[b-1].token; a=b-1; while(a>limit&&faces[a-1].token==token)a--; pos=a; }
     /* Graphemes and surrogate pairs have one face, so these boundaries cannot
      * land inside a code point or a cluster. */
     char *span=NULL; int32_t span_len=0;
     if(!utf16_to_utf8(u,a,b-a,&span,&span_len)) {ec=U_MEMORY_ALLOCATION_ERROR;break;}
     TTFShapeFace face=faces[a];
     if(!shape_span(&face,span,(size_t)span_len,rtl,byte_at[a],&all,&n,&cap)) ec=U_INTERNAL_PROGRAM_ERROR;
     free(span); if(U_FAILURE(ec))break;
   }
 }
 ubidi_close(bidi); free(u); free(byte_at); free(faces); free(cp_u16); free(cps); free(preferred);
 if(U_FAILURE(ec)||!n) {free(all);return 0;} *out=all; *outn=n; return 1;
}
int ttf_shape_text_resolved_clusters(const char *txt,size_t len,TTFShapeResolve resolver,
                                     TTFShapeClusterResolve cluster_resolver,void *ctx,
                                     TTFShapedGlyph **out,size_t *outn) {
 return ttf_shape_text_resolved_clusters_bidi(txt,len,resolver,cluster_resolver,ctx,-1,0,out,outn);
}
int ttf_shape_text_resolved(const char *txt,size_t len,TTFShapeResolve resolver,void *ctx,TTFShapedGlyph **out,size_t *outn) {
 return ttf_shape_text_resolved_clusters(txt,len,resolver,NULL,ctx,out,outn);
}
typedef struct { TTFShapeFace face; } SingleFace;
static int single_resolve(void *v,uint32_t cp,TTFShapeFace *out) { (void)cp; *out=((SingleFace*)v)->face; out->covered=1; return 1; }
int ttf_shape_text(const uint8_t *data,size_t data_len,int upem,const char *txt,size_t len,TTFShapedGlyph **out,size_t *outn) {
 SingleFace sf; sf.face.data=data; sf.face.data_len=data_len; sf.face.upem=upem; sf.face.token=0; sf.face.covered=1;
 return ttf_shape_text_resolved(txt,len,single_resolve,&sf,out,outn);
}
void ttf_shape_free(TTFShapedGlyph *p){free(p);}
#else
int ttf_shape_text_resolved(const char*t,size_t l,TTFShapeResolve r,void*c,TTFShapedGlyph**o,size_t*n){(void)t;(void)l;(void)r;(void)c;if(o)*o=NULL;if(n)*n=0;return 0;}
int ttf_shape_text_resolved_clusters(const char*t,size_t l,TTFShapeResolve r,TTFShapeClusterResolve cr,void*c,TTFShapedGlyph**o,size_t*n){(void)t;(void)l;(void)r;(void)cr;(void)c;if(o)*o=NULL;if(n)*n=0;return 0;}
int ttf_shape_text_resolved_clusters_bidi(const char*t,size_t l,TTFShapeResolve r,TTFShapeClusterResolve cr,void*c,int d,int b,TTFShapedGlyph**o,size_t*n){(void)t;(void)l;(void)r;(void)cr;(void)c;(void)d;(void)b;if(o)*o=NULL;if(n)*n=0;return 0;}
int ttf_shape_text(const uint8_t*d,size_t dl,int upem,const char*t,size_t l,TTFShapedGlyph**o,size_t*n){(void)d;(void)dl;(void)upem;(void)t;(void)l;if(o)*o=NULL;if(n)*n=0;return 0;}
int ttf_bidi_reorder_segments_scoped(const char *const*s,const size_t*l,const TTFBidiScope*sc,size_t c,int d,size_t*m){(void)s;(void)l;(void)sc;(void)d;if(!m||c==0)return 0;for(size_t i=0;i<c;i++)m[i]=i;return 1;}
int ttf_bidi_reorder_segments_ex(const char *const*s,const size_t*l,const uint8_t*md,size_t c,int d,size_t*m){if(!c||c>SIZE_MAX/sizeof(TTFBidiScope))return 0;TTFBidiScope*sc=calloc(c,sizeof(*sc));if(!sc)return 0;if(md)for(size_t i=0;i<c;i++)if(md[i]){sc[i].count=1;sc[i].modes[0]=md[i];sc[i].ids[0]=(uintptr_t)i+1;}int ok=ttf_bidi_reorder_segments_scoped(s,l,sc,c,d,m);free(sc);return ok;}
int ttf_bidi_reorder_segments(const char *const*s,const size_t*l,size_t c,int d,size_t*m){return ttf_bidi_reorder_segments_ex(s,l,NULL,c,d,m);}
void ttf_shape_free(TTFShapedGlyph*p){free(p);}
#endif
