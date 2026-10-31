#include "wasm_binary.h"
#include <stdio.h>
#include <string.h>

static int fail(char *err, size_t cap, const char *why) {
    if (err && cap) snprintf(err, cap, "%s", why);
    return 0;
}

static int valid_utf8(const uint8_t *p, size_t n) {
    size_t i = 0;
    while (i < n) {
        uint8_t a = p[i++];
        if (a < 0x80) continue;
        if (a >= 0xc2 && a <= 0xdf) {
            if (i >= n || (p[i] & 0xc0) != 0x80) return 0;
            i++;
        } else if (a >= 0xe0 && a <= 0xef) {
            if (i + 1 >= n) return 0;
            uint8_t b = p[i], c = p[i + 1];
            if ((b & 0xc0) != 0x80 || (c & 0xc0) != 0x80) return 0;
            if ((a == 0xe0 && b < 0xa0) || (a == 0xed && b >= 0xa0)) return 0;
            i += 2;
        } else if (a >= 0xf0 && a <= 0xf4) {
            if (i + 2 >= n) return 0;
            uint8_t b = p[i], c = p[i + 1], d = p[i + 2];
            if ((b & 0xc0) != 0x80 || (c & 0xc0) != 0x80 || (d & 0xc0) != 0x80) return 0;
            if ((a == 0xf0 && b < 0x90) || (a == 0xf4 && b >= 0x90)) return 0;
            i += 3;
        } else return 0;
    }
    return 1;
}

static int read_u32_leb(const uint8_t *p, size_t end, size_t *cursor,
                        uint32_t *value) {
    uint32_t out = 0;
    unsigned shift = 0;
    for (unsigned i = 0; i < 5; ++i) {
        if (*cursor >= end) return 0;
        uint8_t b = p[(*cursor)++];
        if (i == 4 && (b & 0xf0u)) return 0; /* u32 overflow */
        out |= (uint32_t)(b & 0x7fu) << shift;
        if (!(b & 0x80u)) {
            *value = out;
            return 1;
        }
        shift += 7;
    }
    return 0;
}

/* Convert binary section IDs into their prescribed ordering. The DataCount
 * section (12) precedes Code (10); Tag (13) precedes Global (6). */
static int section_rank(uint8_t id) {
    switch (id) {
        case 1: return 1;  /* Type */
        case 2: return 2;  /* Import */
        case 3: return 3;  /* Function */
        case 4: return 4;  /* Table */
        case 5: return 5;  /* Memory */
        case 13: return 6; /* Tag */
        case 6: return 7;  /* Global */
        case 7: return 8;  /* Export */
        case 8: return 9;  /* Start */
        case 9: return 10; /* Element */
        case 12: return 11;/* DataCount */
        case 10: return 12;/* Code */
        case 11: return 13;/* Data */
        default: return 0;
    }
}

int mlb_wasm_binary_preflight(const uint8_t *p, size_t n,
                              char *err, size_t cap) {
    static const uint8_t magic_version[8] = {0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00};
    if (err && cap) err[0] = '\0';
    if (!p) return fail(err, cap, "WebAssembly bytes are null");
    if (n < sizeof magic_version) return fail(err, cap, "WebAssembly binary header is truncated");
    if (memcmp(p, magic_version, sizeof magic_version) != 0)
        return fail(err, cap, "Invalid WebAssembly magic or unsupported binary version");

    size_t pos = sizeof magic_version;
    unsigned seen = 0;
    int last_rank = 0;
    while (pos < n) {
        uint8_t id = p[pos++];
        uint32_t payload_len = 0;
        if (id > 13) return fail(err, cap, "Unknown WebAssembly section id");
        if (!read_u32_leb(p, n, &pos, &payload_len))
            return fail(err, cap, "Malformed WebAssembly section length");
        if ((size_t)payload_len > n - pos)
            return fail(err, cap, "WebAssembly section extends beyond the module");
        size_t section_end = pos + (size_t)payload_len;

        if (id == 0) {
            /* A custom section starts with a length-prefixed UTF-8 name. */
            if (pos >= section_end)
                return fail(err, cap, "WebAssembly custom section has no name");
            uint32_t name_len = 0;
            if (!read_u32_leb(p, section_end, &pos, &name_len))
                return fail(err, cap, "Malformed WebAssembly custom section name");
            if ((size_t)name_len > section_end - pos)
                return fail(err, cap, "WebAssembly custom section name is truncated");
            if (!valid_utf8(p + pos, name_len))
                return fail(err, cap, "WebAssembly custom section name is not valid UTF-8");
            /* Skip name and arbitrary custom payload; unknown custom sections
             * are valid and must not affect standard-section ordering. */
        } else {
            int rank = section_rank(id);
            if (!rank) return fail(err, cap, "Unsupported WebAssembly section id");
            unsigned bit = 1u << id;
            if (seen & bit) return fail(err, cap, "Duplicate WebAssembly section");
            if (rank <= last_rank) return fail(err, cap, "WebAssembly sections are out of order");
            seen |= bit;
            last_rank = rank;
        }
        pos = section_end;
    }
    return 1;
}
