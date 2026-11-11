#!/usr/bin/env bash
# Cross compiles OrbitaXR.exe for x64 and arm64 with llvm-mingw, from Linux, WSL or Git Bash on Windows.
#   LLVM_MINGW=/path/to/llvm-mingw ./build.sh
set -euo pipefail
cd "$(dirname "$0")"
if [ -n "${LLVM_MINGW:-}" ]; then export PATH="$LLVM_MINGW/bin:$PATH"; fi
if ! command -v x86_64-w64-mingw32-clang++ >/dev/null; then
    echo "llvm-mingw not found, run it as: LLVM_MINGW=/path/to/llvm-mingw ./build.sh" >&2
    exit 1
fi

mkdir -p build/gen
# LICENSE and the notices are shown on the About page
{
    printf '#pragma once\nstatic const char* kLicenses = R"__('
    cat LICENSE
    printf '\n\n'
    cat THIRD_PARTY_NOTICES.md
    printf ')__";\n'
} > build/gen/licenses.h

SOURCES=(src/core/*.cpp src/win/*.cpp)
LIBS=(-ld3d11 -ldxgi -luser32 -lgdi32 -lshell32 -lcomctl32 -lole32 -lsetupapi -lcfgmgr32 -lws2_32 -ladvapi32 -ldwmapi)

for arch in x86_64 aarch64; do
    out=build/$arch
    mkdir -p "$out"
    defs=()
    [ "$arch" = aarch64 ] && defs=(-DARM64_BUILD)
    $arch-w64-mingw32-windres "${defs[@]}" -I src/win/res -O coff src/win/res/app.rc -o "$out/res.o"
    $arch-w64-mingw32-clang++ -std=c++17 -O2 -municode -mwindows -static -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 \
        -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-cast-function-type-mismatch \
        -Ibuild/gen "${SOURCES[@]}" "$out/res.o" -o "$out/OrbitaXR.exe" "${LIBS[@]}" -s
    echo "built $out/OrbitaXR.exe"
done

# the files that go on a GitHub release
mkdir -p build/release
cp build/x86_64/OrbitaXR.exe build/release/OrbitaXR-x64.exe
cp build/aarch64/OrbitaXR.exe build/release/OrbitaXR-arm64.exe
(cd build/release && sha256sum OrbitaXR-x64.exe OrbitaXR-arm64.exe | tee SHA256SUMS.txt)
