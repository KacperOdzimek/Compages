# Binarium Compages Font
Is a Binarium Version 0 File for fonts storage.

## Archive Contents

| Entry Name | Type | Description |
| - | - | - |
|format| UTF-8 | Identifier: "BinariumCompagesFont" |
|base_size |float64	| Font base size.
|ascent	| float64	| Font ascent.
|descent |	float64	| Font descent.
|line_gap |	float64	| Font line gap.
|texture| Compressed Binary | Grayscale atlas texture (1 byte per pixel).
|texture_width	|Int64 |	Atlas width in pixels.
|texture_height	|Int64 |	Atlas height in pixels.
|glyphs	|Compressed Binary | Array of glyph records.
|kerning| Compressed Binary | Array of kerning pair records.

## Glyphs Binary Format
All little endian

```cpp
struct glyph_entry {
    uint32  codepoint;

    float32 uv_min_x;
    float32 uv_min_y;

    float32 uv_max_x;
    float32 uv_max_y;

    float32 size_x;
    float32 size_y;

    float32 bearing_x;
    float32 bearing_y;

    float32 advance_x;
};
```

# Kerning Binary Format
All little endian

```c
struct kerning_entry {
    uint32  left_codepoint;
    uint32  right_codepoint;
    float32 advance_x;
};
```
