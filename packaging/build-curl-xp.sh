#!/bin/sh
# Build a Windows-XP-compatible, statically linked curl.exe (mbedTLS) with the
# same mingw toolchain that builds ANACHRON, so the release zip can carry its
# own modern-TLS transport. XP's schannel lacks the ECDHE+GCM cipher suites
# modern sites (github.com!) require; curl+mbedTLS does TLS in-process.
#
# Produces dist/curl-xp/: curl.exe, ca-bundle.crt (copied from the build host's
# trust store), CURL-LICENSE.txt, MBEDTLS-LICENSE.txt.
#
# Pinned sources (checksums verified):
#   curl 7.88.1   - the last curl generation with first-class Windows XP support
#   mbedTLS 2.28.8 - LTS series; entropy via CryptGenRandom (advapi32, XP-safe)
#
# The result is import-audited: one _s-family CRT symbol or Vista+ API and the
# build FAILS (that exact class of bug shipped ANACHRON v0.15.0 unloadable).
set -e
cd "$(dirname "$0")/.."

CURL_V=7.88.1
MBED_V=2.28.8
CURL_SHA=cdb38b72e36bc5d33d5b8810f8018ece1baa29a8f215b4495e495ded82bbf3c7
MBED_SHA=4fef7de0d8d542510d726d643350acb3cdb9dc76ad45611b59c9aa08372b4213

W=build-curl-xp
mkdir -p "$W" dist/curl-xp
cd "$W"

[ -f curl.tar.gz ]    || curl -sLo curl.tar.gz "https://curl.se/download/curl-$CURL_V.tar.gz"
[ -f mbedtls.tar.gz ] || curl -sLo mbedtls.tar.gz "https://github.com/Mbed-TLS/mbedtls/archive/refs/tags/v$MBED_V.tar.gz"
echo "$CURL_SHA  curl.tar.gz"    | sha256sum -c - >/dev/null
echo "$MBED_SHA  mbedtls.tar.gz" | sha256sum -c - >/dev/null
[ -d "curl-$CURL_V" ]    || tar xzf curl.tar.gz
[ -d "mbedtls-$MBED_V" ] || tar xzf mbedtls.tar.gz

cat > mingw-xp.cmake <<'EOF'
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR i686)
set(CMAKE_C_COMPILER i686-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER i686-w64-mingw32-g++)
set(CMAKE_RC_COMPILER i686-w64-mingw32-windres)
set(CMAKE_FIND_ROOT_PATH /usr/i686-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
# XP target; SSE2 ceiling; and CRITICALLY: map mbedTLS's printf shims to plain
# C99 snprintf/vsnprintf - its Windows default is vsnprintf_s, which stock XP's
# msvcrt.dll does not export (one missing import = the exe never loads).
set(CMAKE_C_FLAGS_INIT "-D_WIN32_WINNT=0x0501 -DWINVER=0x0501 -msse2 -mno-sse3 -DMBEDTLS_PLATFORM_SNPRINTF_MACRO=snprintf -DMBEDTLS_PLATFORM_VSNPRINTF_MACRO=vsnprintf")
EOF

P="$PWD/prefix"
cmake -S "mbedtls-$MBED_V" -B mbed-build -DCMAKE_TOOLCHAIN_FILE="$PWD/mingw-xp.cmake" \
      -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_BUILD_TYPE=Release \
      -DENABLE_TESTING=OFF -DENABLE_PROGRAMS=OFF -DCMAKE_INSTALL_PREFIX="$P" >/dev/null
cmake --build mbed-build -j4 >/dev/null
cmake --install mbed-build >/dev/null

cmake -S "curl-$CURL_V" -B curl-build -DCMAKE_TOOLCHAIN_FILE="$PWD/mingw-xp.cmake" \
      -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_SHARED_LIBS=OFF -DCURL_USE_MBEDTLS=ON -DCURL_USE_SCHANNEL=OFF \
      -DHTTP_ONLY=ON -DCURL_ZLIB=OFF -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF \
      -DUSE_NGHTTP2=OFF -DCURL_USE_LIBSSH2=OFF -DCURL_USE_LIBPSL=OFF \
      -DCURL_DISABLE_LDAP=ON \
      -DMBEDTLS_INCLUDE_DIRS="$P/include" \
      -DMBEDTLS_LIBRARY="$P/lib/libmbedtls.a" \
      -DMBEDX509_LIBRARY="$P/lib/libmbedx509.a" \
      -DMBEDCRYPTO_LIBRARY="$P/lib/libmbedcrypto.a" \
      -DCMAKE_EXE_LINKER_FLAGS="-static -static-libgcc" >/dev/null
cmake --build curl-build -j4 >/dev/null

EXE=curl-build/src/curl.exe
# Import audit: same discipline as `make xp-audit`, same blocklist spirit.
BAD='_putenv_s|_wputenv_s|getenv_s|_dupenv_s|fopen_s|strcpy_s|strcat_s|strncpy_s|memcpy_s|memmove_s|sprintf_s|_snprintf_s|_vsnprintf_s|vsnprintf_s|_controlfp_s|GetTickCount64|GetSystemTimePreciseAsFileTime|InitializeSRWLock|AcquireSRWLock|ReleaseSRWLock|InitializeConditionVariable|SleepConditionVariable|WakeConditionVariable|WakeAllConditionVariable|InitOnceExecuteOnce|FlsAlloc|FlsFree|FlsGetValue|FlsSetValue|CreateEventEx|CreateSemaphoreEx|CancelIoEx|BCrypt'
if i686-w64-mingw32-objdump -p "$EXE" \
     | grep -E '^[[:space:]]+[0-9a-f]+[[:space:]]+[0-9]+[[:space:]]+[A-Za-z_]' \
     | awk '{print $NF}' | sort -u | grep -Ex "($BAD)"; then
    echo "CURL-XP AUDIT FAIL: post-XP imports found (see above)"; exit 1
fi
if i686-w64-mingw32-objdump -p "$EXE" | grep -qi 'DLL Name: bcrypt'; then
    echo "CURL-XP AUDIT FAIL: bcrypt.dll import (Vista+)"; exit 1
fi

cp "$EXE" ../dist/curl-xp/curl.exe
cp /etc/ssl/certs/ca-certificates.crt ../dist/curl-xp/ca-bundle.crt
cp "curl-$CURL_V/COPYING" ../dist/curl-xp/CURL-LICENSE.txt
cp "mbedtls-$MBED_V/LICENSE" ../dist/curl-xp/MBEDTLS-LICENSE.txt
echo "Built dist/curl-xp/curl.exe (static, mbedTLS, XP-audited) + ca-bundle.crt + licenses."
