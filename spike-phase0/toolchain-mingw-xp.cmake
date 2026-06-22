# CMake toolchain: i686 Windows XP target via mingw-w64 (posix threads, static).
# posix variant required for std::thread/std::mutex in GCC 10 libstdc++.
# winpthreads is statically linked and uses XP-safe primitives (no Vista CONDITION_VARIABLE).
set(CMAKE_SYSTEM_NAME Windows)
# must be i686 (not "x86") so ggml_get_system_arch() matches ^(x86_64|i686|AMD64|amd64)$
# and selects the x86 SSE2 kernels instead of the generic scalar fallback.
set(CMAKE_SYSTEM_PROCESSOR i686)

set(CMAKE_C_COMPILER   i686-w64-mingw32-gcc-posix)
set(CMAKE_CXX_COMPILER i686-w64-mingw32-g++-posix)
set(CMAKE_RC_COMPILER  i686-w64-mingw32-windres)

set(CMAKE_FIND_ROOT_PATH /usr/i686-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# Fully static so the .exe carries libstdc++/libgcc/winpthread (no DLL deps beyond
# KERNEL32 + msvcrt). XP subsystem version 5.01.
set(_XP_LINK "-static -static-libgcc -static-libstdc++ -Wl,--major-subsystem-version=5 -Wl,--minor-subsystem-version=1")
set(CMAKE_EXE_LINKER_FLAGS_INIT    "${_XP_LINK}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_XP_LINK}")
