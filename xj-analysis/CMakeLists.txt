cmake_minimum_required(VERSION 3.17)
project(xj-analysis)

# The must-equality library: a forward dataflow analysis (MustEqual) giving
# the cells provably equal at each program point, plus clients that reduce
# those states to a fact — ResolveParameter answers "does this pointer hold
# the same value as some nameable lvalue at every one of its use sites?".

set(XJ_LLVM_ROOT "${CMAKE_CURRENT_LIST_DIR}/../_local/xj-llvm")

include_directories("${XJ_LLVM_ROOT}/include")

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti")

add_library(xj-analysis STATIC
    Cell.cpp
    SED.cpp
    Escape.cpp
    Transfer.cpp
    MustEqual.cpp
    VarMutation.cpp
    ResolveParameter.cpp
)

set_target_properties(xj-analysis PROPERTIES
    CXX_STANDARD 17
    CXX_EXTENSIONS OFF
)

target_include_directories(xj-analysis PUBLIC "${CMAKE_CURRENT_LIST_DIR}")

# No target_link_libraries on the library itself: a static library leaves
# libclang-cpp / libLLVM to be resolved by whichever executable links it,
# exactly as the xj-prepare-* tools already resolve them.

# The annotation-driven test driver. This is the primary test surface for
# the library; see examples/ and tests/analysis/.
set(XJ_LLVM_LIB_DIR "${XJ_LLVM_ROOT}/lib")
find_library(XJ_LLVM_LIBCLANG
	NAMES libclang${CMAKE_SHARED_LIBRARY_SUFFIX}
	PATHS "${XJ_LLVM_LIB_DIR}" REQUIRED NO_DEFAULT_PATH)
find_library(XJ_LLVM_LIBCLANGCPP
	NAMES libclang-cpp${CMAKE_SHARED_LIBRARY_SUFFIX}
	PATHS "${XJ_LLVM_LIB_DIR}" REQUIRED NO_DEFAULT_PATH)
find_library(XJ_LLVM_LIBLLVM
	NAMES libLLVM${CMAKE_SHARED_LIBRARY_SUFFIX}
	PATHS "${XJ_LLVM_LIB_DIR}" REQUIRED NO_DEFAULT_PATH)

add_executable(xj-musteq-check tools/xj-musteq-check/main.cpp)

set_target_properties(xj-musteq-check PROPERTIES
    CXX_STANDARD 17
    CXX_EXTENSIONS OFF
)

target_link_libraries(xj-musteq-check
    xj-analysis
    ${XJ_LLVM_LIBCLANG}
    ${XJ_LLVM_LIBCLANGCPP}
    ${XJ_LLVM_LIBLLVM}
)
