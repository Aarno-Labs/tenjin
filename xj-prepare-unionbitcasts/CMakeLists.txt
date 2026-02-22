cmake_minimum_required(VERSION 3.17)
project(xj-prepare-unionbitcasts)

set(XJ_LLVM_ROOT "${CMAKE_CURRENT_LIST_DIR}/../_local/xj-llvm")

include_directories("${XJ_LLVM_ROOT}/include")

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti")

add_executable(xj-prepare-unionbitcasts
    main.cpp
    Common.cpp
    UnionAccessCollector.cpp
    FunctionAccessAnalyzer.cpp
    ValidationMethods.cpp
    TransformationMethods.cpp
    UnionTransformAction.cpp
)

set_target_properties(xj-prepare-unionbitcasts PROPERTIES
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

target_link_libraries(xj-prepare-unionbitcasts
	${XJ_LLVM_LIBCLANG}
	${XJ_LLVM_LIBCLANGCPP}
	${XJ_LLVM_LIBLLVM}
)
