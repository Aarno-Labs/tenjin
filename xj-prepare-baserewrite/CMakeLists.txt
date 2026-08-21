cmake_minimum_required(VERSION 3.17)
project(xj-prepare-baserewrite)

set(XJ_LLVM_ROOT "${CMAKE_CURRENT_LIST_DIR}/../_local/xj-llvm")
set(XJ_SUPPORT_DIR "${CMAKE_CURRENT_LIST_DIR}/../xj-prepare-support")

include_directories("${XJ_LLVM_ROOT}/include" "${XJ_SUPPORT_DIR}")

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti")

# The must-equality library this tool is the first consumer of. It is its
# own CMake project, pulled in as a subdirectory rather than installed;
# EXCLUDE_FROM_ALL keeps its two hand-driven check tools out of this
# build, which only wants the library.
#
# Note that xj-analysis/CMakeLists.txt sets the LLVM include path with a
# bare `include_directories`, which does not propagate out of
# add_subdirectory — hence the one above, as in the sibling tools.
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../xj-analysis" xj-analysis EXCLUDE_FROM_ALL)

add_executable(xj-prepare-baserewrite
    main.cpp
    BaseRewriteAction.cpp
    Reconstructor.cpp
    "${XJ_SUPPORT_DIR}/PtrIndexMetadata.cpp"
    "${XJ_SUPPORT_DIR}/FunctionKey.cpp"
)

set_target_properties(xj-prepare-baserewrite PROPERTIES
    CXX_STANDARD 17
    CXX_EXTENSIONS OFF
)

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

target_link_libraries(xj-prepare-baserewrite
	xj-analysis
	${XJ_LLVM_LIBCLANG}
	${XJ_LLVM_LIBCLANGCPP}
	${XJ_LLVM_LIBLLVM}
)
