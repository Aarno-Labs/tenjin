cmake_minimum_required(VERSION 3.17)
project(xj-localize-errno)

find_package(LLVM REQUIRED CONFIG)
find_package(Clang REQUIRED CONFIG)

include_directories(${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS} ${CLANG_DEFINITIONS})

set(CMAKE_BUILD_TYPE Debug)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti")

add_definitions(-DCLANG_LIBDIR_SUFFIX="${LLVM_LIBDIR_SUFFIX}")

add_executable(xj-localize-errno 
  StdlibSpec.cpp
  FindExternal.cpp
  LocalizeErrnoASTVisitor.cpp
  LocalizeErrno.cpp
)

set_target_properties(xj-localize-errno PROPERTIES
  CXX_STANDARD 17
  CXX_EXTENSIONS OFF
  )

target_link_libraries(xj-localize-errno
  libclang
  clang-cpp
  clangTooling
  clangBasic
  clangASTMatchers
  clangTransformer
  LLVM)
