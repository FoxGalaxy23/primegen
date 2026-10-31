#ifndef TTF_SHAPING_H
#define TTF_SHAPING_H
#include <stddef.h>
#include <stdint.h>
typedef struct {
    uint32_t glyph;
    uint32_t cluster; /* UTF-8 byte offset in source string */
    int32_t x_advance, y_advance, x_offset, y_offset; /* font units */
    uintptr_t font_token; /* opaque owner of this glyph; 0 for single-face API */
    uint8_t rtl; /* glyph belongs to an RTL visual run; used for logical caret mapping */
} TTFShapedGlyph;
typedef struct {
    const uint8_t *data;
    size_t data_len;
    int upem;
    uintptr_t token;
    int covered; /* 0 means this face has no cmap entry for the source codepoint */
} TTFShapeFace;
typedef int (*TTFShapeResolve)(void *context, uint32_t codepoint, TTFShapeFace *out_face);
/* Select one font for an entire Unicode grapheme cluster. The callback may
 * inspect all code points and the resolver's font chain to avoid splitting
 * combining sequences, Indic conjuncts, or emoji ZWJ sequences across faces. */
typedef int (*TTFShapeClusterResolve)(void *context, const uint32_t *codepoints,
                                      size_t codepoint_count, TTFShapeFace *out_face);
/* 1 = shaped, 0 = optional shaping backend unavailable/unsupported/error. */
int ttf_shape_text(const uint8_t *font_data, size_t font_len, int upem,
                   const char *utf8, size_t utf8_len,
                   TTFShapedGlyph **out_glyphs, size_t *out_count);
/* Bidi-aware shaping that resolves a face for each Unicode scalar. The resolver
 * is called in logical source order; token identifies the renderer's TTFFont. */
int ttf_shape_text_resolved(const char *utf8, size_t utf8_len,
                            TTFShapeResolve resolver, void *context,
                            TTFShapedGlyph **out_glyphs, size_t *out_count);
int ttf_shape_text_resolved_clusters(const char *utf8, size_t utf8_len,
                                     TTFShapeResolve resolver,
                                     TTFShapeClusterResolve cluster_resolver,
                                     void *context,
                                     TTFShapedGlyph **out_glyphs, size_t *out_count);
/* Direction-aware variant: base_direction is -1 (first-strong/default LTR),
 * 0 (explicit LTR), or 1 (explicit RTL). bidi_override forces each paragraph
 * character to the chosen direction, as required by unicode-bidi:bidi-override. */
int ttf_shape_text_resolved_clusters_bidi(const char *utf8, size_t utf8_len,
                                          TTFShapeResolve resolver,
                                          TTFShapeClusterResolve cluster_resolver,
                                          void *context, int base_direction,
                                          int bidi_override,
                                          TTFShapedGlyph **out_glyphs, size_t *out_count);
/* Reorder discrete inline text segments for one already-wrapped line.
 * Each segment is one visual/layout item (word, normalized space, etc.).
 * visual_to_logical[v] gives the source segment index for visual slot v.
 * base_direction: -1=first strong/default LTR, 0=LTR, 1=RTL.
 * Returns 1 on success; the no-ICU build returns identity order. */
#define TTF_BIDI_MAX_SCOPE_DEPTH 16
typedef struct {
    uint8_t count;
    uint8_t modes[TTF_BIDI_MAX_SCOPE_DEPTH]; /* explicit scopes, outermost first */
    uintptr_t ids[TTF_BIDI_MAX_SCOPE_DEPTH]; /* stable identity of each inline scope */
} TTFBidiScope;
enum {
    TTF_BIDI_SEGMENT_NORMAL = 0,
    TTF_BIDI_SEGMENT_EMBED_LTR = 1,
    TTF_BIDI_SEGMENT_EMBED_RTL = 2,
    TTF_BIDI_SEGMENT_OVERRIDE_LTR = 3,
    TTF_BIDI_SEGMENT_OVERRIDE_RTL = 4,
    TTF_BIDI_SEGMENT_ISOLATE_LTR = 5,
    TTF_BIDI_SEGMENT_ISOLATE_RTL = 6,
    TTF_BIDI_SEGMENT_ISOLATE_FIRST_STRONG = 7,
    TTF_BIDI_SEGMENT_ISOLATE_OVERRIDE_LTR = 8,
    TTF_BIDI_SEGMENT_ISOLATE_OVERRIDE_RTL = 9
};
int ttf_bidi_reorder_segments_scoped(const char *const *segments, const size_t *lengths,
                                     const TTFBidiScope *segment_scopes,
                                     size_t segment_count, int base_direction,
                                     size_t *visual_to_logical);
int ttf_bidi_reorder_segments_ex(const char *const *segments, const size_t *lengths,
                                 const uint8_t *segment_modes,
                                 size_t segment_count, int base_direction,
                                 size_t *visual_to_logical);
int ttf_bidi_reorder_segments(const char *const *segments, const size_t *lengths,
                              size_t segment_count, int base_direction,
                              size_t *visual_to_logical);
/* Given logical caret x offsets (one per UTF-8 byte boundary), return the nearest
 * legal UTF-8/grapheme boundary. Boundaries with identical adjacent caret x are
 * skipped so a hit test does not split combining/zero-advance clusters. */
int ttf_hit_test_byte_offset(const char *utf8, size_t utf8_len,
                             const int *caret_offsets, size_t offset_count,
                             float x, size_t *out_byte_offset);
typedef struct {
    float x, y, height;
    int row; /* visual-line row, ordered top-to-bottom */
} TTFVisualCaretStop;
/* Move one caret stop in visual screen order. direction: -1 left, +1 right,
 * -2 up, +2 down. Stops are ordered by row then x. preferred_x is retained
 * for vertical movement. Returns 1 when a different stop exists. */
int ttf_visual_caret_step(const TTFVisualCaretStop *stops, size_t count,
                          size_t current, int direction, float preferred_x,
                          size_t *out_next);
/* Unicode-aware word navigation helper. Returns the next logical word boundary
 * in one UTF-8 string (direction -1=previous word start, +1=next word start). */
int ttf_text_word_step(const char *utf8, size_t utf8_len, size_t byte_offset,
                       int direction, size_t *out_byte_offset);
int ttf_unicode_is_word_codepoint(uint32_t codepoint);
typedef struct {
    const char *utf8;       /* source text node; remains owned by DOM */
    size_t byte_offset;     /* logical UTF-8 boundary */
    int paragraph_id;       /* stable id for the current paragraph/block */
} TTFLogicalCaretPosition;
int ttf_logical_caret_move_word(const TTFLogicalCaretPosition *positions, size_t count,
                                size_t current, int direction, size_t *out_target);
int ttf_logical_caret_move_paragraph(const TTFLogicalCaretPosition *positions, size_t count,
                                     size_t current, int direction, size_t *out_target);
void ttf_shape_free(TTFShapedGlyph *glyphs);
#endif
