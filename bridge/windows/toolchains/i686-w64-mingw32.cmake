set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR i686)

set(CMAKE_C_COMPILER /opt/homebrew/bin/i686-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER /opt/homebrew/bin/i686-w64-mingw32-g++)
set(CMAKE_RC_COMPILER /opt/homebrew/bin/i686-w64-mingw32-windres)
set(CMAKE_AR /opt/homebrew/bin/i686-w64-mingw32-ar)
set(CMAKE_RANLIB /opt/homebrew/bin/i686-w64-mingw32-ranlib)
set(CMAKE_C_COMPILER_AR /opt/homebrew/bin/i686-w64-mingw32-gcc-ar)
set(CMAKE_C_COMPILER_RANLIB /opt/homebrew/bin/i686-w64-mingw32-gcc-ranlib)
set(CMAKE_CXX_COMPILER_AR /opt/homebrew/bin/i686-w64-mingw32-gcc-ar)
set(CMAKE_CXX_COMPILER_RANLIB /opt/homebrew/bin/i686-w64-mingw32-gcc-ranlib)

# Avoid executable link checks during compiler detection; cross-linking
# is validated by the actual DLL target build instead.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_FIND_ROOT_PATH /opt/homebrew/opt/mingw-w64/toolchain-i686 /usr/local/opt/mingw-w64/toolchain-i686)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
