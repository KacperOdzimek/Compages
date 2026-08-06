/*
----------------------------------------------------------------
Contents:
This file provides a `font` object, which consists of glyph SDF texture atlas and glyph metrics.
The font object is created from a binarium-compages-font file, which can be generated out of other font formats, using
    Compages/formats/binarium_compages_font

----------------------------------------------------------------
Code info:
- cmg_fnt prefix
- COMPAGES_FONT_IMPL macro to build
- fundatio/graphics.h dependant
- binarium/binarium.h dependant

----------------------------------------------------------------
Usage:
- Create font object, with a valid binarium-compages-font linked in create info
- Lookup info->out fields to get atlas position within file; upload to font texture (no offset, full dimensions)
- Use get functions to query font/glyph metrics
- cmg_fnt_get_glyph and cmg_fnt_get_kerning are O(log n) operations
- rest of get operations are O(1)

----------------------------------------------------------------
Possible Optimizations:
- instead of binary searching glyphs, binary search over continuous ranges of glyphs, 
    perform O(1) array access within range - this would be faster
*/

#ifndef COMPAGES_FONT_H
#define COMPAGES_FONT_H

#include "fundatio/platform/graphics.h"
#include "binarium/binarium.h"
#include <stddef.h>

typedef struct cmg_fnt_glyph {
    fnd_gfx_uv_2d   atlas_position;
    float           size_x;
    float           size_y;
    float           bearing_x;
    float           bearing_y;
    float           advance_x;
} cmg_fnt_glyph;

typedef struct cmg_fnt_create_info {
    biu_view*       file_view;          // The binarium compages file view
    uint64_t*       out_bytes;          // Byte size of sdf texture
    unsigned char** out_sdf_texture;    // Mallocated decompressed texture to be uploaded
} cmg_fnt_create_info;

typedef struct cmg_fnt_font cmg_fnt_font;
cmg_fnt_font* cmg_fnt_create_font(fnd_gfx_hardware*, const cmg_fnt_create_info*);
void cmg_fnt_free_font(cmg_fnt_font*);

fnd_gfx_texture* cmg_fnt_font_get_texture(const cmg_fnt_font*);

float cmg_fnt_font_get_base_size    (const cmg_fnt_font*);
float cmg_fnt_font_get_base_ascent  (const cmg_fnt_font*);
float cmg_fnt_font_get_base_descent (const cmg_fnt_font*);
float cmg_fnt_font_get_base_line_gap(const cmg_fnt_font*);

cmg_fnt_glyph    cmg_fnt_font_get_glyph  (const cmg_fnt_font*, uint32_t codepoint);
float            cmg_fnt_font_get_kerning(const cmg_fnt_font*, uint32_t left_codepoint, uint32_t right_codepoint);

// UTF8 iteration helper, returns pointer advance
static inline int cmg_fnt_utf8_decode(const char* str, size_t itr, uint32_t* codepoint) {
    str += itr; unsigned char c = (unsigned char)str[0];

    if (c < 0x80) {
        *codepoint = c;
        return 1;
    }
    else if ((c >> 5) == 0x6) {
        *codepoint = ((c & 0x1F) << 6) | (str[1] & 0x3F);
        return 2;
    }
    else if ((c >> 4) == 0xE) {
        *codepoint = ((c & 0x0F) << 12) | ((str[1] & 0x3F) << 6) | (str[2] & 0x3F);
        return 3;
    }
    else if ((c >> 3) == 0x1E) {
        *codepoint = ((c & 0x07) << 18) | ((str[1] & 0x3F) << 12) | ((str[2] & 0x3F) << 6) | (str[3] & 0x3F);
        return 4;
    }

    // invalid fallback
    *codepoint = '?';
    return 1;
}

#endif // COMPAGES_FONT_H

#ifdef COMPAGES_FONT_IMPL

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef struct glyph_entry {
    uint32_t    codepoint;
    cmg_fnt_glyph glyph;
} glyph_entry;

typedef struct kerning_pair_entry {
    uint32_t    left_codepoint;
    uint32_t    right_codepoint;
    float       advance_x;
} kerning_pair_entry;

struct cmg_fnt_font {
    fnd_gfx_hardware*       owning_hardware;

    float               size;
    float               ascent;
    float               descent;
    float               line_gap;

    uint32_t            glyphs_count;
    glyph_entry*        glyphs_array;

    uint32_t            kernings_count;
    kerning_pair_entry* kernings_array;

    fnd_gfx_texture*        atlas_texture;
};

// deserialize little-endian 32-bit value
static inline uint32_t deserialize_reg_32(const unsigned char* b) {
    return (uint32_t)(
        ((uint32_t)b[0])       |
        ((uint32_t)b[1] << 8)  |
        ((uint32_t)b[2] << 16) |
        ((uint32_t)b[3] << 24)
    );
}

// Helper function to read a 32-bit float from little-endian bytes
static inline float deserialize_f32(const unsigned char* b) {
    uint32_t bits = deserialize_reg_32(b);
    float val;
    memcpy(&val, &bits, sizeof(val));
    return val;
}

cmg_fnt_font* cmg_fnt_create_font(fnd_gfx_hardware* hardware, const cmg_fnt_create_info* info) {
    biu_view* view = info->file_view; uint32_t idx = 0;

    cmg_fnt_font* font = (cmg_fnt_font*)calloc(1, sizeof(cmg_fnt_font));
    if (!font) return NULL;
    font->owning_hardware = hardware;

    // Validate format identifier
    const uint8_t* format_str = NULL; uint64_t format_len = 0;
    if (biu_view_find(view, "format", &idx) != biu_status_ok ||
        biu_view_get_as_bytes(view, idx, &format_len, &format_str) != biu_status_ok ||
        format_len != strlen("BinariumCompagesFont") ||
        memcmp(format_str, "BinariumCompagesFont", format_len) != 0) {
        goto _fail;
    }

    double d_val = 0.0; int64_t i_val = 0;

    // Read Font Base Metrics
    if (biu_view_find(view, "base_size", &idx) == biu_status_ok &&
        biu_view_get_as_float64(view, idx, &d_val) == biu_status_ok) {
        font->size = (float)d_val;
    } else goto _fail;

    if (biu_view_find(view, "ascent", &idx) == biu_status_ok &&
        biu_view_get_as_float64(view, idx, &d_val) == biu_status_ok) {
        font->ascent = (float)d_val;
    } else goto _fail;

    if (biu_view_find(view, "descent", &idx) == biu_status_ok &&
        biu_view_get_as_float64(view, idx, &d_val) == biu_status_ok) {
        font->descent = (float)d_val;
    } else goto _fail;

    if (biu_view_find(view, "line_gap", &idx) == biu_status_ok &&
        biu_view_get_as_float64(view, idx, &d_val) == biu_status_ok) {
        font->line_gap = (float)d_val;
    } else goto _fail;

    // Read Atlas Dimensions
    int64_t tex_w = 0, tex_h = 0;
    if (biu_view_find(view, "texture_width", &idx) != biu_status_ok ||
        biu_view_get_as_int64(view, idx, &tex_w) != biu_status_ok) goto _fail;

    if (biu_view_find(view, "texture_height", &idx) != biu_status_ok ||
        biu_view_get_as_int64(view, idx, &tex_h) != biu_status_ok) goto _fail;

    if (tex_w > UINT32_MAX || tex_h > UINT32_MAX) goto _fail;

    // Decompress Texture Data
    if (biu_view_find(view, "texture", &idx) == biu_status_ok) {
        uint64_t uncompressed_size = 0;
        if (biu_view_get_as_uncompressed_size(view, idx, &uncompressed_size) != biu_status_ok) goto _fail;

        unsigned char* sdf_tex = (unsigned char*)malloc(uncompressed_size);
        if (!sdf_tex) goto _fail;

        if (biu_view_get_as_decompress(view, idx, uncompressed_size, sdf_tex) != biu_status_ok) {
            free(sdf_tex);
            goto _fail;
        }

        // Return decompressed texture buffer and byte size through out pointers
        if (info->out_sdf_texture) *info->out_sdf_texture = sdf_tex;
        if (info->out_bytes)       *info->out_bytes = uncompressed_size;
    }

    // Decompress and Parse Glyphs Array
    if (biu_view_find(view, "glyphs", &idx) == biu_status_ok) {
        uint64_t uncompressed_size = 0;
        if (biu_view_get_as_uncompressed_size(view, idx, &uncompressed_size) != biu_status_ok) goto _fail;

        uint8_t* raw_glyphs = (uint8_t*)malloc(uncompressed_size);
        if (!raw_glyphs) goto _fail;

        if (biu_view_get_as_decompress(view, idx, uncompressed_size, raw_glyphs) != biu_status_ok) {
            free(raw_glyphs);
            goto _fail;
        }

        const size_t raw_entry_size = 40; // 1*uint32 (4) + 9*float32 (36)
        uint32_t count = (uint32_t)(uncompressed_size / raw_entry_size);

        font->glyphs_count = count;
        font->glyphs_array = (glyph_entry*)calloc(count, sizeof(glyph_entry));
        if (!font->glyphs_array) {
            free(raw_glyphs);
            goto _fail;
        }

        for (uint32_t i = 0; i < count; ++i) {
            const uint8_t* ptr = raw_glyphs + (i * raw_entry_size);
            font->glyphs_array[i].codepoint                  = deserialize_reg_32(ptr + 0);
            font->glyphs_array[i].glyph.atlas_position.min_x = deserialize_f32(ptr + 4);
            font->glyphs_array[i].glyph.atlas_position.min_y = deserialize_f32(ptr + 8);
            font->glyphs_array[i].glyph.atlas_position.max_x = deserialize_f32(ptr + 12);
            font->glyphs_array[i].glyph.atlas_position.max_y = deserialize_f32(ptr + 16);
            font->glyphs_array[i].glyph.size_x               = deserialize_f32(ptr + 20);
            font->glyphs_array[i].glyph.size_y               = deserialize_f32(ptr + 24);
            font->glyphs_array[i].glyph.bearing_x            = deserialize_f32(ptr + 28);
            font->glyphs_array[i].glyph.bearing_y            = deserialize_f32(ptr + 32);
            font->glyphs_array[i].glyph.advance_x            = deserialize_f32(ptr + 36);
        }

        free(raw_glyphs);
    }

    // Decompress and Parse Kerning Pairs
    if (biu_view_find(view, "kerning", &idx) != biu_status_ok) goto _fail;
    uint64_t uncompressed_size = 0;
    if (biu_view_get_as_uncompressed_size(view, idx, &uncompressed_size) == biu_status_ok && uncompressed_size > 0) {
        uint8_t* raw_kerning = (uint8_t*)malloc(uncompressed_size);
        if (!raw_kerning) goto _fail;

        if (biu_view_get_as_decompress(view, idx, uncompressed_size, raw_kerning) != biu_status_ok) {
            free(raw_kerning);
            goto _fail;
        }

        const size_t raw_entry_size = 12; // 2*uint32 (8) + 1*float32 (4)
        uint32_t count = (uint32_t)(uncompressed_size / raw_entry_size);

        font->kernings_count = count;
        font->kernings_array = (kerning_pair_entry*)calloc(count, sizeof(kerning_pair_entry));
        if (!font->kernings_array) {
            free(raw_kerning);
            goto _fail;
        }

        for (uint32_t i = 0; i < count; ++i) {
            const uint8_t* ptr = raw_kerning + (i * raw_entry_size);
            font->kernings_array[i].left_codepoint  = deserialize_reg_32(ptr + 0);
            font->kernings_array[i].right_codepoint = deserialize_reg_32(ptr + 4);
            font->kernings_array[i].advance_x      = deserialize_f32(ptr + 8);
        }

        free(raw_kerning);
    }

    // Create texture
    font->atlas_texture = fnd_gfx_create_texture(hardware, &(fnd_gfx_texture_create_info){
        .type   = fnd_gfx_texture_type_2d,
        .usage  = fnd_gfx_texture_usage_sampled,
        .dimensions = (fnd_gfx_texture_dimensions){
            .width  = tex_w,
            .height = tex_h,
            .depth  = 1
        },
        .mipmap_layers  = 1,
        .array_length   = 1,
        .format         = fnd_gfx_texture_format_r8_unorm,
        .memory_access  = fnd_gfx_memory_access_staging_write
    }); if (!font->atlas_texture) goto _fail;

    return font;
_fail:
    cmg_fnt_free_font(font); return NULL;
}

void cmg_fnt_free_font(cmg_fnt_font* font) {
    if (font == NULL) return;
    free(font->glyphs_array);
    free(font->kernings_array);
    fnd_gfx_free_texture(font->atlas_texture);
    free(font);
}

fnd_gfx_texture* cmg_fnt_font_get_texture(const cmg_fnt_font* font) {
    return font->atlas_texture;
}

cmg_fnt_glyph cmg_fnt_font_get_glyph(const cmg_fnt_font* font, uint32_t codepoint) {
    int left = 0;
    int right = (int)font->glyphs_count - 1;

    while (left <= right) {
        int mid = left + (right - left) / 2;
        uint32_t mid_codepoint = font->glyphs_array[mid].codepoint;

        if (mid_codepoint == codepoint) return font->glyphs_array[mid].glyph;
        if (mid_codepoint < codepoint)  left  = mid + 1;
        else                            right = mid - 1;
    }

    // fallback: missing glyph (return empty / zero glyph)
    return (cmg_fnt_glyph){0};
}

float cmg_fnt_font_get_kerning(
    const cmg_fnt_font* font,
    uint32_t left_codepoint,
    uint32_t right_codepoint
) {
    uint64_t key = ((uint64_t)left_codepoint << 32) | (uint64_t)right_codepoint;
    int left = 0;
    int right = (int)font->kernings_count - 1;

    while (left <= right) {
        int mid = left + (right - left) / 2;

        uint64_t mid_key =
            ((uint64_t)font->kernings_array[mid].left_codepoint << 32) |
             (uint64_t)font->kernings_array[mid].right_codepoint;

        if (mid_key == key) return font->kernings_array[mid].advance_x;
        if (mid_key < key)  left  = mid + 1;
        else                right = mid - 1;
    }

    return 0.0f;
}

float cmg_fnt_font_get_base_size(const cmg_fnt_font* font) {
    return font->size;
}

float cmg_fnt_font_get_base_ascent(const cmg_fnt_font* font) {
    return font->ascent;
}

float cmg_fnt_font_get_base_descent(const cmg_fnt_font* font) {
    return font->descent;
}

float cmg_fnt_font_get_base_line_gap(const cmg_fnt_font* font) {
    return font->line_gap;
}

#endif // COMPAGES_FONT_IMPL
