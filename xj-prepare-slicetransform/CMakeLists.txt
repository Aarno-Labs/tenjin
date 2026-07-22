cmake_minimum_required(VERSION 3.17)
project(xj-prepare-slicetransform)

set(XJ_LLVM_ROOT "${CMAKE_CURRENT_LIST_DIR}/../_local/xj-llvm")
set(XJ_SUPPORT_DIR "${CMAKE_CURRENT_LIST_DIR}/../xj-prepare-support")

include_directories("${XJ_LLVM_ROOT}/include" "${XJ_SUPPORT_DIR}")

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti")

add_executable(xj-prepare-slicetransform
    main.cpp
    SliceTransformAction.cpp
    SliceRewriter.cpp
    "${XJ_SUPPORT_DIR}/PtrIndexMetadata.cpp"
)

set_target_properties(xj-prepare-slicetransform PROPERTIES
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

target_link_libraries(xj-prepare-slicetransform
	${XJ_LLVM_LIBCLANG}
	${XJ_LLVM_LIBCLANGCPP}
	${XJ_LLVM_LIBLLVM}
)
