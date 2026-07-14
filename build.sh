#!/bin/sh
# build.sh -- build the shared object + CLI with gcc/clang (Linux/macOS).
# Produces build/libwavelet.so (or .dylib) and build/wavelet
set -e

CC="${CC:-cc}"
DIR="$(cd "$(dirname "$0")" && pwd)"
OUT="$DIR/build"
mkdir -p "$OUT"

case "$(uname -s)" in
  Darwin) LIB="libwavelet.dylib"; SONAME="-install_name @rpath/$LIB" ;;
  *)      LIB="libwavelet.so";    SONAME="-Wl,-soname,$LIB" ;;
esac

echo "compiling shared library -> $OUT/$LIB"
"$CC" -O2 -Wall -Wextra -std=c99 -fPIC -fvisibility=hidden -DWV_BUILD_SHARED \
      -I"$DIR/include" -shared $SONAME \
      "$DIR/src/wavelet.c" -o "$OUT/$LIB" -lm

echo "compiling CLI -> $OUT/wavelet"
"$CC" -O2 -Wall -Wextra -std=c99 \
      -I"$DIR/include" \
      "$DIR/src/cli.c" -L"$OUT" -lwavelet -Wl,-rpath,"$OUT" \
      -o "$OUT/wavelet" -lm

echo "compiling self-test -> $OUT/wavelet_test"
"$CC" -O2 -Wall -Wextra -std=c99 -DWV_STATIC \
      -I"$DIR/include" \
      "$DIR/test/test.c" "$DIR/src/wavelet.c" \
      -o "$OUT/wavelet_test" -lm

echo "done: $OUT/$LIB, $OUT/wavelet, $OUT/wavelet_test"
