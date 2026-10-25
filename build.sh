#!/bin/sh
# Builds build/xinput1_3.dll (32-bit, like Obscure.exe). Needs an i686 MinGW gcc or clang on PATH,
# e.g. llvm-mingw (https://github.com/mstorsjo/llvm-mingw/releases).
set -e
cd "$(dirname "$0")"
mkdir -p build
i686-w64-mingw32-gcc -O2 -Wall -Wextra -shared -static -static-libgcc -o build/xinput1_3.dll src/obscoop.c src/obscoop.def \
	-Wl,--kill-at -lws2_32
sha256sum build/xinput1_3.dll
