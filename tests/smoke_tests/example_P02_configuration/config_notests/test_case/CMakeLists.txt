# Defaults
set(APP_MODE  "fast"  CACHE STRING "Operation mode (fast, safe)")
set(BACKEND   "alpha" CACHE STRING "Backend implementation (alpha, beta)")
set(WORD_SIZE "32"    CACHE STRING "Word size in bits (32, 64)")
option(ENABLE_EXTRA "Build and link the extra module" OFF)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wpedantic")

# Quoted string literal: -DAPP_MODE_STR="fast"
add_compile_definitions("APP_MODE_STR=\"${APP_MODE}\"")

# Bare integer token: -DWORD_SIZE=32
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DWORD_SIZE=${WORD_SIZE}")

# String composition
set(BUILD_PROFILE "${BACKEND}_${WORD_SIZE}")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DBUILD_PROFILE=${BUILD_PROFILE}")

# ${BACKEND} expands at configure time. Only one of backend_alpha.c / backend_beta.c is compiled.
set(BACKEND_SOURCES src/backend_${BACKEND}.c)
add_library(backend STATIC ${BACKEND_SOURCES})

# Enables cross-directory #include "params.h" and #include "backend.h" without relative paths in the consuming source files.
target_include_directories(backend PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Conditional library
if(ENABLE_EXTRA)
    add_library(extra STATIC src/extra.c)
    target_link_libraries(extra PUBLIC backend)
    target_compile_definitions(extra PRIVATE ENABLE_EXTRA)
endif()

# Executable
add_executable(skeleton src/main.c)

if(ENABLE_EXTRA)
    target_compile_definitions(skeleton PRIVATE ENABLE_EXTRA)
    target_link_libraries(skeleton PRIVATE extra)
endif()

target_link_libraries(skeleton PRIVATE backend)