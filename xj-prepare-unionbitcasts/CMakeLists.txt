
cmake_minimum_required(VERSION 3.17)
project(xj-prepare-unionbitcasts)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
add_compile_options(-frtti)

find_package(LLVM REQUIRED CONFIG)
find_package(Clang REQUIRED CONFIG)

include_directories(${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

add_executable(xj-prepare-unionbitcasts
    main.cpp
    Common.cpp
    UnionAccessCollector.cpp
    FunctionAccessAnalyzer.cpp
    ValidationMethods.cpp
    TransformationMethods.cpp
    UnionTransformAction.cpp
)


target_link_libraries(xj-prepare-unionbitcasts libclang clang-cpp LLVM)
