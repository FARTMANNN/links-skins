#!/bin/sh
# Builds mod/link_skins.dll for Windows from any OS. Needs python3 with pip, and git.
set -e
cd "$(dirname "$0")"
python3 -m pip install --quiet ziglang
[ -d zstd ] || git clone --quiet --depth 1 --branch v1.5.6 https://github.com/facebook/zstd.git
Z=zstd/lib
python3 -m ziglang cc -target x86_64-windows-gnu -shared -O2 -s \
    -DZSTD_DISABLE_ASM -DZSTD_MULTITHREAD=0 -I$Z -Isrc \
    src/link_skins.c $Z/common/*.c $Z/compress/*.c $Z/decompress/*.c -o mod/link_skins.dll
echo "built mod/link_skins.dll"
