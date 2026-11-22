#!/bin/sh
# Builds the small FFmpeg the plugin needs for the game's movies: only the Bink demuxer, the Bink video and
# audio decoders and the scaler, as static libraries.
#   build_ffmpeg.sh <ffmpeg source> <build dir> <install prefix> [android <ndk>]
set -e
SRC=$(realpath "$1"); BUILD=$2; PREFIX=$3
COMMON="--disable-everything --disable-programs --disable-doc --disable-network --disable-autodetect
  --disable-avdevice --disable-avfilter --disable-swresample --enable-swscale --enable-static --disable-shared
  --enable-pic --enable-demuxer=bink --enable-decoder=bink,binkaudio_rdft,binkaudio_dct --enable-protocol=file"
mkdir -p "$BUILD" && cd "$BUILD"
if [ "$4" = android ]; then
  TC=$5/toolchains/llvm/prebuilt/linux-x86_64/bin
  "$SRC/configure" $COMMON --prefix="$PREFIX" --enable-cross-compile --target-os=android --arch=aarch64 \
    --cc=$TC/aarch64-linux-android24-clang --cxx=$TC/aarch64-linux-android24-clang++ --ar=$TC/llvm-ar \
    --nm=$TC/llvm-nm --ranlib=$TC/llvm-ranlib --strip=$TC/llvm-strip
else
  "$SRC/configure" $COMMON --prefix="$PREFIX" --disable-x86asm
fi
make -j"$(nproc)" install
