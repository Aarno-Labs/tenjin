cmake_minimum_required(VERSION 3.5...4.0)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
project(ASTExporter)

#################################################
# TinyCBOR                                      #
#################################################

set(WITH_CBOR2JSON VALUE CACHE BOOL OFF FORCE)
add_subdirectory(tinycbor)
include_directories(${CMAKE_BINARY_DIR}/tinycbor)

#################################################

set(AST_EXPORTER_SRCS
  AstExporter.cpp
  FloatingLexer.cpp
  ExportResult.cpp
  )

set(AST_EXPORTER_BIN_SRCS
  ${AST_EXPORTER_SRCS}
  Main.cpp
  )

find_package(LLVM REQUIRED CONFIG)

# Debian and Ubuntu's clang cmake files are broken, so we can't require the
# package here. We already have to manually order the link against the clang
# libs in build.rs, so that's not so bad.
find_package(Clang CONFIG)

include_directories(${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS} ${CLANG_DEFINITIONS})

if (DEFINED CLANG_INSTALL_PREFIX)
  add_definitions(-DCLANG_BIN_PATH="${CLANG_INSTALL_PREFIX}/bin")
elseif(DEFINED LLVM_INSTALL_PREFIX)
  add_definitions(-DCLANG_BIN_PATH="${LLVM_INSTALL_PREFIX}/bin")
elseif(DEFINED LLVM_TOOLS_BINARY_DIR)
  add_definitions(-DCLANG_BIN_PATH="${LLVM_TOOLS_BINARY_DIR}")
else()
  message(FATAL_ERROR "Cannot find path to clang binary")
endif()
add_definitions(-DCLANG_VERSION_STRING="${LLVM_PACKAGE_VERSION}")

set(LLVM_LINK_COMPONENTS support)

# LLVM is not always built with RTTI, we don't need it either.
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti")

# The executable
add_executable(c2rust-ast-exporter ${AST_EXPORTER_BIN_SRCS})

# The library
add_library(clangAstExporter STATIC ${AST_EXPORTER_SRCS})

add_definitions(-DCLANG_LIBDIR_SUFFIX="${LLVM_LIBDIR_SUFFIX}")

set_target_properties(c2rust-ast-exporter PROPERTIES
  CXX_STANDARD 17
  CXX_EXTENSIONS OFF
  )
# PRIVATE was added to make c2rust-ast-exporter build with LLVM 6.0. Keyword
# description: https://cmake.org/pipermail/cmake/2016-May/063400.html
target_link_libraries(c2rust-ast-exporter PRIVATE
  clangAST
  clangFrontend
  clangTooling
  clangBasic
  clangASTMatchers
  tinycbor
  )

set_target_properties(clangAstExporter PROPERTIES
  CXX_STANDARD 17 # will decay to 14 if compiler doesn't support c++17
  CXX_EXTENSIONS OFF
  )
target_link_libraries(clangAstExporter PRIVATE
  clangAST
  clangFrontend
  clangTooling
  clangBasic
  clangASTMatchers
  tinycbor
  )

# Make sure the linker can see the locally-built library.
target_link_directories(clangAstExporter    PUBLIC ${CMAKE_BINARY_DIR}/tinycbor)
target_link_directories(c2rust-ast-exporter PUBLIC ${CMAKE_BINARY_DIR}/tinycbor)
