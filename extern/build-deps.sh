#!/usr/bin/env bash
# Builds everything in extern/ for the Wii, so `make` can build boot.dol.
# Run from anywhere:  ./extern/build-deps.sh
#
#   1. Applies the Wii patches to moonlight-common-c and enet (git submodules)
#   2. Mbed TLS 3.6.1 (git submodule)  -> extern/mbedtls-install
#   3. Opus 1.5.2 (downloaded tarball) -> extern/opus-install
#   4. FFmpeg H.264 decoder (in-tree)  -> extern/ffmpeg/lib*/lib*.a
set -euo pipefail

: "${DEVKITPPC:?DEVKITPPC is not set. Install devkitPro (wii-dev) and open a new shell.}"
EXT="$(cd "$(dirname "$0")" && pwd)"
export PATH="$DEVKITPPC/bin:$PATH"
JOBS="$(nproc 2>/dev/null || echo 4)"

OPUS_VERSION=1.5.2
OPUS_SHA256=65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1
OPUS_URL="https://downloads.xiph.org/releases/opus/opus-$OPUS_VERSION.tar.gz"

need_submodules() {
    echo "extern/$1 is empty. Run:  git submodule update --init --recursive" >&2
    exit 1
}

# Applies a patch once: skipped if it is already applied.
apply_patch() {   # <repo dir> <patch file>
    if git -C "$1" apply --reverse --check "$2" 2>/dev/null; then
        echo "   already applied: $(basename "$2")"
    else
        git -C "$1" apply "$2"
        echo "   applied: $(basename "$2")"
    fi
}

echo "== [1/4] Wii patches for moonlight-common-c"
[ -f "$EXT/moonlight-common-c/src/Limelight.h" ] || need_submodules moonlight-common-c
[ -f "$EXT/moonlight-common-c/enet/protocol.c" ] || need_submodules moonlight-common-c/enet
apply_patch "$EXT/moonlight-common-c" "$EXT/../patches/moonlight-common-c.patch"
apply_patch "$EXT/moonlight-common-c/enet" "$EXT/../patches/enet.patch"

echo "== [2/4] Mbed TLS"
[ -f "$EXT/mbedtls/CMakeLists.txt" ] || need_submodules mbedtls
cmake -S "$EXT/mbedtls" -B "$EXT/mbedtls-build" \
    -DCMAKE_SYSTEM_NAME=Generic \
    -DCMAKE_C_COMPILER="$DEVKITPPC/bin/powerpc-eabi-gcc" \
    -DCMAKE_C_FLAGS="-mcpu=750 -meabi -mhard-float -Wno-error=unterminated-string-initialization" \
    -DMBEDTLS_USER_CONFIG_FILE="$EXT/mbedtls_wii_config.h" \
    -DENABLE_PROGRAMS=Off -DENABLE_TESTING=Off \
    -DCMAKE_INSTALL_PREFIX="$EXT/mbedtls-install"
cmake --build "$EXT/mbedtls-build" -j"$JOBS"
cmake --install "$EXT/mbedtls-build"

echo "== [3/4] Opus $OPUS_VERSION"
mkdir -p "$EXT/opus-build"
cd "$EXT/opus-build"
[ -f "opus-$OPUS_VERSION.tar.gz" ] || curl -fL -o "opus-$OPUS_VERSION.tar.gz" "$OPUS_URL"
echo "$OPUS_SHA256  opus-$OPUS_VERSION.tar.gz" | sha256sum -c -
rm -rf "opus-$OPUS_VERSION"
tar xzf "opus-$OPUS_VERSION.tar.gz"
cd "opus-$OPUS_VERSION"
# Fixed-point build: the configuration the app was developed and tested with.
./configure --host=powerpc-eabi --prefix="$EXT/opus-install" \
    --disable-shared --enable-static --disable-doc --disable-extra-programs \
    --disable-asm --disable-rtcd --disable-intrinsics --enable-fixed-point \
    CFLAGS="-O2 -DGEKKO -mrvl -mcpu=750 -meabi -mhard-float"
make -j"$JOBS"
make install

echo "== [4/4] FFmpeg (H.264 decoder only)"
make -C "$EXT/ffmpeg" -j"$JOBS" \
    libavutil/libavutil.a libavcodec/libavcodec.a libswscale/libswscale.a

echo
echo "Done. Now run 'make' in the repository root to build boot.dol."
