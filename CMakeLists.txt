cmake_minimum_required(VERSION 3.16)
project(LuauDecompiler LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(LUAU_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/third_party/luau)

# Optionally expose the upstream Luau CLI tools (luau, luau-compile) as targets of this
# build tree. They are excluded from ALL so the decompiler builds quickly on its own:
#   cmake --build build --target luau-compile luau
option(LUAUDEC_WITH_LUAU_TOOLS "Add third_party/luau as a subdirectory (targets luau, luau-compile)" ON)
if(LUAUDEC_WITH_LUAU_TOOLS AND EXISTS ${LUAU_ROOT}/CMakeLists.txt)
    set(LUAU_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(LUAU_BUILD_WEB OFF CACHE BOOL "" FORCE)
    add_subdirectory(${LUAU_ROOT} EXCLUDE_FROM_ALL)
endif()

add_executable(luaudec
    src/main.cpp
    src/bytecode/Bytecode.cpp
    src/disasm/Disasm.cpp
    src/cfg/CFG.cpp
    src/lift/Lift.cpp
    src/emit/Emit.cpp
)

target_include_directories(luaudec PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${LUAU_ROOT}/Common/include
)

if(MSVC)
    target_compile_options(luaudec PRIVATE /W4 /wd4100 /wd4458 /utf-8)
    target_compile_definitions(luaudec PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(luaudec PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
