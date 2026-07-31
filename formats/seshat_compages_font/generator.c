/*
    This file generates SeshatCompagesFont (scf) from other font formats.
    Supported right now:
    - truefont
*/

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>

#define STB_RECT_PACK_IMPLEMENTATION
#include "../depedency/stb_rect_pack.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "../depedency/stb_truetype.h"

#define SESHAT_IMPL
#include "seshat/seshat.h"

// Conifg

uint32_t texture_width  = 256;
uint32_t texture_height = 256;

const float font_size       = 48.0f;
const float font_dist_scale = 36.0f;
const int   font_onedge     = 180;
const int   font_padding    = 5;

// Typedefs

typedef struct glyph_info {
    float uv_min_x, uv_min_y;
    float uv_max_x, uv_max_y;
    float size_x;
    float size_y;
    float bearing_x;
    float bearing_y;
    float advance_x;
} glyph_info;

typedef struct glyph_entry {
    uint32_t    codepoint;
    glyph_info  glyph;
} glyph_entry;
const uint32_t glyph_entry_serialized_size = 4 + 10 * 4;

typedef struct kerning_pair_entry {
    uint32_t    left_codepoint;
    uint32_t    right_codepoint;
    float       advance_x;
} kerning_pair_entry;
const uint32_t kerning_entry_serialized_size = 2 * 4 + 4;

// Generated 

uint32_t            glyph_entires_count;
glyph_entry*        glyph_entires;

uint32_t            kerning_entries_count;
kerning_pair_entry* kerning_entries;

uint32_t            atlas_texture_width;
uint32_t            atlas_texture_height;
unsigned char*      atlas_texture;

float               font_base_size;
float               font_ascent;
float               font_descent;
float               font_line_gap;

// Helper, flips texture in Y
void flip_y(unsigned char *data, int w, int h) {
    int row_size = w;

    unsigned char *temp_row = (unsigned char*)malloc(row_size);
    for (int y = 0; y < h / 2; y++) {
        unsigned char *row_top    = data + y * row_size;
        unsigned char *row_bottom = data + (h - 1 - y) * row_size;

        memcpy(temp_row, row_top, row_size);
        memcpy(row_top, row_bottom, row_size);
        memcpy(row_bottom, temp_row, row_size);
    }

    free(temp_row);
}

// Code

int generate();
int serialize();

int main() {
    if (generate()) {
        fprintf(stderr, "Failed to generate demigurg font\n");
        return 1;
    }
    if (serialize()) {
        fprintf(stderr, "Failed to save demigurg font\n");
        return 1;
    }
    printf("Success!");
    return 0;
}

int generate() {
    // Open source file
    FILE* file; {
        char font_path[256];
        fprintf(stderr, "Enter font path: ");
        scanf("%255s", font_path);

        file = fopen(font_path, "rb");
        if (!file) {
            fprintf(stderr, "Could not open file\n");
            return 1;
        }
    }

    // Read file
    unsigned char* font_data; {
        fseek(file, 0, SEEK_END);
        size_t font_size_bytes = ftell(file);
        fseek(file, 0, SEEK_SET);
        font_data = malloc(font_size_bytes);
        fread(font_data, 1, font_size_bytes, file);
        fclose(file);
    }

    // Ask for codepoint ranges (begin end), when begin > end finish
    uint32_t  cp_count = 0;
    uint32_t* cp_list = NULL; {
        fprintf(stderr, "Enter ranges (begin end), or (1 0) to finish: ");
        while (1) {
            uint32_t start, end;
            scanf("%u %u", &start, &end);
            if (start > end) break;
            for (uint32_t i = start; i <= end; i++) {
                cp_list = realloc(cp_list, sizeof(uint32_t) * (cp_count + 1));
                cp_list[cp_count++] = i;
            }
        }
        if (cp_count == 0) return 0;
    }

    // Read font
    stbtt_fontinfo stbfont; stbtt_InitFont(&stbfont, font_data, 0);
    float scale = stbtt_ScaleForPixelHeight(&stbfont, font_size);

    // Generate glyphs array

    // all per glyph data required in process
    typedef struct extra_glyph_data {
        uint32_t        codepoint;
        int             glyph_index;
        int             sdf_width;
        int             sdf_height;
        unsigned char*  sdf_texture;
    } extra_glyph_data;
    extra_glyph_data* egds = calloc(cp_count, sizeof(extra_glyph_data));
    
    // all-good glyphs
    uint32_t     valid_glyphs = 0;
    glyph_entry* glyphs = calloc(cp_count, sizeof(glyph_entry));

    // generate data for atlas packing
    uint32_t    valid_to_pack = 0; // valid glyphs - invisible glyphs like spaces
    stbrp_rect* pack_rects = calloc(cp_count, sizeof(stbrp_rect));
    
    for (uint32_t i = 0; i < cp_count; i++) {
        glyph_entry*        entry = &glyphs[valid_glyphs];
        extra_glyph_data*   egd   = &egds[valid_glyphs];

        egd->codepoint   = cp_list[i];
        egd->glyph_index = stbtt_FindGlyphIndex(&stbfont, egd->codepoint);
        if (egd->glyph_index == 0) {
            printf("No such glyph in source font: %"PRId32". Glyph ommited.\n", egd->codepoint);
            continue;
        }

        int w, h, off_x, off_y;
        egd->sdf_texture = stbtt_GetGlyphSDF(
            &stbfont,
            scale,
            egd->glyph_index,
            font_padding,
            font_onedge,
            font_dist_scale,
            &w, &h,
            &off_x, &off_y
        );
        egd->sdf_width  = w;
        egd->sdf_height = h;

        int advance; int lbearing;
        stbtt_GetGlyphHMetrics(&stbfont, stbtt_FindGlyphIndex(&stbfont, egd->codepoint), &advance, &lbearing);

        // Handle invisible nodes
        if (!egd->sdf_texture) {
            entry->codepoint = egd->codepoint;
            entry->glyph = (glyph_info){
                .size_x     = 0,
                .size_y     = 0,
                .bearing_x  = 0,
                .bearing_y  = 0,
                .advance_x  = ((float)advance) * scale,
            };

            valid_glyphs++;
            continue;
        }

        // Flip texture
        flip_y(egd->sdf_texture, egd->sdf_width, egd->sdf_height);

        // Generate font entry

        entry->codepoint = egd->codepoint;
        entry->glyph = (glyph_info){
            .size_x     = w,
            .size_y     = h,
            .bearing_x  = off_x,
            .bearing_y  = off_y,
            .advance_x  = ((float)advance) * scale
        };

        // Enqueue packing

        pack_rects[valid_to_pack] = (stbrp_rect){
            .w = w, 
            .h = h,
            .id = (int)egd->codepoint
        };

        valid_glyphs++;
        valid_to_pack++;
    }

    // Try to pack atlas using stb rect pack
    // on failure double texture size
    while (1) {
        stbrp_context   context;
        stbrp_node*     nodes = malloc(sizeof(stbrp_node) * texture_width * 2);
        stbrp_init_target(&context, texture_width, texture_height, nodes, texture_width);

        if (stbrp_pack_rects(&context, pack_rects, valid_to_pack)) {
            free(nodes); break;
        } 
        else {
            texture_width  *= 2;
            texture_height *= 2;
        }

        free(nodes);
    }

    // Generate Reverse Glyph -> Codepoint lookup
    uint32_t  glyph_count = stbfont.numGlyphs;
    uint32_t* glyph_to_cp = calloc(glyph_count, sizeof(uint32_t));

    for (uint32_t i = 0; i < cp_count; i++) {
        uint32_t cp = cp_list[i];
        int glyph = stbtt_FindGlyphIndex(&stbfont, cp);
        // preserve first mapping only
        if (glyph > 0 && glyph_to_cp[glyph] == 0) {
            glyph_to_cp[glyph] = cp;
        }
    }

    // Generate Kerning Table
    int kerning_table_length = stbtt_GetKerningTableLength(&stbfont);
    stbtt_kerningentry* table = malloc(kerning_table_length * sizeof(stbtt_kerningentry));
    stbtt_GetKerningTable(&stbfont, table, kerning_table_length);

    uint32_t            valid_kernings = 0;
    kerning_pair_entry* kernings = malloc(kerning_table_length * sizeof(kerning_pair_entry));

    for (int i = 0; i < kerning_table_length; i++) {
        uint32_t left_cp  = glyph_to_cp[table[i].glyph1];
        uint32_t right_cp = glyph_to_cp[table[i].glyph2];

        // skip pairs involving glyphs we don't export
        if (left_cp == 0 || right_cp == 0) continue;

        kernings[valid_kernings].left_codepoint     = left_cp;
        kernings[valid_kernings].right_codepoint    = right_cp;
        kernings[valid_kernings].advance_x          = table[i].advance * scale;

        valid_kernings++;
    }

    // Generate Texture Atlas
    unsigned char* atlas = calloc(texture_width * texture_height, 1);
    uint32_t packed_i = 0;
    for (uint32_t i = 0; i < valid_glyphs; i++) {
        glyph_entry*        entry   = &glyphs[i];
        extra_glyph_data*   egd     = &egds[i];

        if (egd->sdf_texture == NULL) {
            entry->glyph.uv_min_x = 0; entry->glyph.uv_max_x = 0;
            entry->glyph.uv_min_y = 0; entry->glyph.uv_max_y = 0;
            continue; // blank character like space
        } 

        stbrp_rect rect = pack_rects[packed_i];
        packed_i++;

        for (int j = 0; j < egd->sdf_height; j++) 
            memcpy(&atlas[(rect.y + j) * texture_width + rect.x], &egd->sdf_texture[j * egd->sdf_width], egd->sdf_width);
        stbtt_FreeSDF(egd->sdf_texture, NULL);

        entry->glyph.uv_min_x  = (float)rect.x / texture_width;
        entry->glyph.uv_min_y  = (float)rect.y / texture_height;
        entry->glyph.uv_max_x  = (float)(rect.x + rect.w) / texture_width;
        entry->glyph.uv_max_y  = (float)(rect.y + rect.h) / texture_height;
    }

    // Setup generated output
    glyph_entires_count = valid_glyphs;
    glyph_entires = glyphs;

    kerning_entries_count = valid_kernings;
    kerning_entries = kernings;

    atlas_texture_width  = texture_width;
    atlas_texture_height = texture_height;
    atlas_texture = atlas;

    int ascent, descent, linegap;
    stbtt_GetFontVMetrics(&stbfont, &ascent, &descent, &linegap);
    font_base_size  = font_size;
    font_ascent     = (float)ascent  * scale;
    font_descent    = (float)descent * scale;
    font_line_gap   = (float)linegap * scale;

    // Cleanup
    free(font_data);
    free(cp_list);
    free(egds);
    free(pack_rects);
    free(glyph_to_cp);
    free(table);

    return 0;
}

#include <stdalign.h>

#define HELPER_GET_BYTE(reg, byte_num) ((((uint64_t)(reg)) >> (byte_num * 8)) & 0xFF)

// save little endian
static inline uint32_t little_endian_u32(uint32_t v) {
    unsigned char b[] = {
        HELPER_GET_BYTE(v, 0),
        HELPER_GET_BYTE(v, 1),
        HELPER_GET_BYTE(v, 2),
        HELPER_GET_BYTE(v, 3)
    };
    return *(uint32_t*)b;
}

int serialize(void) {
    // Ask for output file
    FILE* out;
    {
        char out_path[256];
        fprintf(stderr, "Enter output file path: ");
        scanf("%255s", out_path);

        out = fopen(out_path, "wb");
        if (!out) {
            fprintf(stderr, "Could not open output file\n");
            return 1;
        }
    }

    sht_builder* builder = NULL;
    if (sht_create_builder(&builder) != sht_status_ok) {
        fprintf(stderr, "Failed to create builder\n");
        fclose(out);
        return 1;
    }

    // ---------------------------------------------------------------------
    // Serialize glyph table

    const uint64_t glyph_record_size =
        4 +     // codepoint
        9 * 4;  // floats

    const uint64_t glyphs_bytes = glyph_entires_count * glyph_record_size;

    uint8_t* glyphs_buffer = malloc(glyphs_bytes);
    if (!glyphs_buffer) {
        fprintf(stderr, "Failed to allocate glyph buffer\n");
        sht_free_builder(builder);
        fclose(out);
        return 1;
    }

    uint64_t pos = 0;

#define WRITE_U32(buf, value)                                              \
    do {                                                                    \
        uint32_t v = little_endian_u32((uint32_t)(value));                  \
        memcpy((buf) + pos, &v, 4);                                         \
        pos += 4;                                                           \
    } while (0)

#define WRITE_F32(buf, value)                                               \
    do {                                                                    \
        float f = (value);                                                  \
        uint32_t bits;                                                      \
        memcpy(&bits, &f, 4);                                               \
        WRITE_U32(buf, bits);                                               \
    } while (0)

    for (uint32_t i = 0; i < glyph_entires_count; ++i) {
        glyph_info g = glyph_entires[i].glyph;

        WRITE_U32(glyphs_buffer, glyph_entires[i].codepoint);

        WRITE_F32(glyphs_buffer, g.uv_min_x);
        WRITE_F32(glyphs_buffer, g.uv_min_y);

        WRITE_F32(glyphs_buffer, g.uv_max_x);
        WRITE_F32(glyphs_buffer, g.uv_max_y);

        WRITE_F32(glyphs_buffer, g.size_x);
        WRITE_F32(glyphs_buffer, g.size_y);

        WRITE_F32(glyphs_buffer, g.bearing_x);
        WRITE_F32(glyphs_buffer, g.bearing_y);

        WRITE_F32(glyphs_buffer, g.advance_x);
    }

    // ---------------------------------------------------------------------
    // Serialize kerning table

    const uint64_t kerning_record_size = 12;
    const uint64_t kerning_bytes =
        kerning_entries_count * kerning_record_size;

    uint8_t* kerning_buffer = malloc(kerning_bytes);
    if (!kerning_buffer) {
        fprintf(stderr, "Failed to allocate kerning buffer\n");
        free(glyphs_buffer);
        sht_free_builder(builder);
        fclose(out);
        return 1;
    }

    pos = 0;

    for (uint32_t i = 0; i < kerning_entries_count; ++i) {
        WRITE_U32(kerning_buffer, kerning_entries[i].left_codepoint);
        WRITE_U32(kerning_buffer, kerning_entries[i].right_codepoint);
        WRITE_F32(kerning_buffer, kerning_entries[i].advance_x);
    }

#undef WRITE_U32
#undef WRITE_F32

    // ---------------------------------------------------------------------
    // Archive entries

    sht_builder_add_text(
        builder,
        "format",
        strlen("SeshatCompagesFont"),
        "SeshatCompagesFont",
        sht_access_read_unowned
    );

    sht_builder_add_float64(builder, "base_size", font_base_size);
    sht_builder_add_float64(builder, "ascent",    font_ascent);
    sht_builder_add_float64(builder, "descent",   font_descent);
    sht_builder_add_float64(builder, "line_gap",  font_line_gap);

    sht_builder_add_int64(builder, "texture_width",  atlas_texture_width);
    sht_builder_add_int64(builder, "texture_height", atlas_texture_height);

    sht_builder_add_binary_compressed(
        builder,
        "texture",
        (uint64_t)atlas_texture_width * atlas_texture_height,
        atlas_texture,
        0
    );

    sht_builder_add_binary_compressed(
        builder,
        "glyphs",
        glyphs_bytes,
        glyphs_buffer,
        0
    );

    sht_builder_add_binary_compressed(
        builder,
        "kerning",
        kerning_bytes,
        kerning_buffer,
        0
    );

    // ---------------------------------------------------------------------
    // Serialize archive

    void* archive = NULL;
    uint64_t archive_size = 0;

    if (sht_builder_serialize(builder, &archive, &archive_size) != sht_status_ok) {
        fprintf(stderr, "Failed to serialize archive\n");

        free(glyphs_buffer);
        free(kerning_buffer);
        sht_free_builder(builder);
        fclose(out);
        return 1;
    }

    fwrite(archive, archive_size, 1, out);

    fclose(out);

    free(archive);
    free(glyphs_buffer);
    free(kerning_buffer);
    sht_free_builder(builder);

    free(glyph_entires);
    free(kerning_entries);
    free(atlas_texture);

    return 0;
}
