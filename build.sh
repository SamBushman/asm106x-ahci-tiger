#!/bin/sh
# Build a Tiger PPC kext with Apple gcc 4.0.1 -- run ON the G5.  Usage: sh build.sh <srcdir> <Name> [extra .cpp ...]
# Recipe copied from atiradeonx1000-kext-source/Tools/issue147_probe/build.sh (proven on this machine).
set -e
SRC=$(cd "$1" && pwd); NAME=$2
OUT=${OUT:-$HOME/rung3/asm1062/build}/$NAME
KFW=/System/Library/Frameworks/Kernel.framework/Headers
GCCLIB=/usr/lib/gcc/powerpc-apple-darwin8/4.0.1
COMMON="-arch ppc -static -fno-common -mlongcall -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -I$KFW -I$SRC ${OPT--Os} ${WARN--Wall -Wno-unused}"
CXXFLAGS="$COMMON -fno-rtti -fno-exceptions -fapple-kext"
sudo rm -rf "$OUT"; mkdir -p "$OUT/obj"
for f in "$SRC"/*.cpp; do g++ $CXXFLAGS -c "$f" -o "$OUT/obj/$(basename "$f" .cpp).o"; done
gcc $COMMON -c "$SRC/kmod_info.c" -o "$OUT/kmod_info.o"
g++ -arch ppc -nostdlib -static -r -o "$OUT/$NAME" "$OUT/kmod_info.o" "$OUT"/obj/*.o -L$GCCLIB -lkmodc++ -lkmod -lcc_kext
nm -u "$OUT/$NAME" | sed 's/^ *//' | sort -u > "$OUT/undef.txt"
mkdir -p "$OUT/$NAME.kext/Contents/MacOS"
cp "$SRC/Info.plist" "$OUT/$NAME.kext/Contents/Info.plist"
cp "$OUT/$NAME" "$OUT/$NAME.kext/Contents/MacOS/$NAME"
sudo chown -R root:wheel "$OUT/$NAME.kext"; sudo chmod -R go-w "$OUT/$NAME.kext"
echo "bundle: $OUT/$NAME.kext"
