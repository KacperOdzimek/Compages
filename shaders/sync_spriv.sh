#!/usr/bin/env bash

set -euo pipefail

SRC_DIR="glsl"
DST_DIR="spirv"

# Remove previous output
rm -rf "$DST_DIR"
mkdir -p "$DST_DIR"

# Compile every file under glsl/
find "$SRC_DIR" -type f | while IFS= read -r src; do
    rel="${src#$SRC_DIR/}"
    dst="$DST_DIR/$rel.spirv"

    mkdir -p "$(dirname "$dst")"

    echo "Compiling $src -> $dst"
    glslc "$src" -o "$dst"
done

echo "Done."
