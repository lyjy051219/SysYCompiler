cmake_minimum_required(VERSION 3.13)
project(compiler LANGUAGES CXX)

# 必须在顶层启用，子目录注册的后端测试才能被根构建目录的 CTest 发现。
enable_testing()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# === 构建选项 ===
#如果需要使用哪个版本进行编译就去找相关的指令即可
option(BUILD_FRONTEND_ONLY "只构建编译器前端用于调试" OFF)
option(BUILD_MIDDLE_END "构建前端和中端(IR)用于调试" ON)
option(BUILD_FRONTEND_RELEASE "构建前端Release版本(-O2)" OFF)
# 选择后端目标架构：默认 RISC-V；打开 BUILD_AARCH64_BACKEND 则构建 ARM64 后端。
option(BUILD_AARCH64_BACKEND "构建 ARM64(AArch64) 后端而非 RISC-V 后端" OFF)

# 用来clang-tidy做代码检查
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
find_program(CLANG_TIDY_EXE NAMES "clang-tidy")

if(CLANG_TIDY_EXE)
    message(STATUS "Found clang-tidy: ${CLANG_TIDY_EXE}")
    set(CMAKE_CXX_CLANG_TIDY
        "${CLANG_TIDY_EXE};--warnings-as-errors=*;--format-style=file"
    )
else()
    message(WARNING "clang-tidy not found! Code checks will be skipped.")
endif()

#包含通用的头文件目录utils
include_directories(${PROJECT_SOURCE_DIR}/include/utils)

if(BUILD_FRONTEND_ONLY)
    message(STATUS "=== 构建前端调试版本 ===")
    set(CMAKE_CXX_CLANG_TIDY "")  # 关闭代码检查以简化构建

    add_subdirectory(${PROJECT_SOURCE_DIR}/src/lib)
    add_subdirectory(${PROJECT_SOURCE_DIR}/src/yacc)

    # 前端可执行文件
    add_executable(compiler_frontend ${CMAKE_SOURCE_DIR}/main.cpp)
    target_compile_definitions(compiler_frontend PRIVATE
    RUNTIME_DIR="${CMAKE_CURRENT_SOURCE_DIR}/runtime"
    )

    target_link_libraries(compiler_frontend Lib Yacc)

    # 调试选项
    target_compile_options(compiler_frontend PRIVATE
        -g
        -O0
        -Wall
        -Wextra
    )

elseif(BUILD_FRONTEND_RELEASE)
    message(STATUS "=== 构建前端Release版本 (-O2) ===")
    set(CMAKE_CXX_CLANG_TIDY "")

    add_subdirectory(${PROJECT_SOURCE_DIR}/src/lib)
    add_subdirectory(${PROJECT_SOURCE_DIR}/src/yacc)

    # 目标文件名改为 compiler_frontend_release
    add_executable(compiler_frontend_release ${CMAKE_SOURCE_DIR}/main.cpp)
    target_compile_definitions(compiler_frontend_release PRIVATE
        RUNTIME_DIR="${CMAKE_CURRENT_SOURCE_DIR}/runtime"
    )

    # 链接 m 库 (对应 -lm)
    target_link_libraries(compiler_frontend_release Lib Yacc m)

    # 编译选项：-O2 (优化), -g (生成符号表供 lldb 使用，但代码会被优化)
    target_compile_options(compiler_frontend_release PRIVATE
        -O2
        -g
        -Wall
        -Wextra
    )


elseif(BUILD_MIDDLE_END)
    message(STATUS "=== 构建中端调试版本 ===")
    set(CMAKE_CXX_CLANG_TIDY "") # 关闭代码检查以简化构建

    add_subdirectory(${PROJECT_SOURCE_DIR}/src/lib)
    add_subdirectory(${PROJECT_SOURCE_DIR}/src/yacc)
    add_subdirectory(${PROJECT_SOURCE_DIR}/src/ir)

    # 中端可执行文件
    add_executable(compiler_middle_end ${CMAKE_SOURCE_DIR}/main.cpp)
    target_compile_definitions(compiler_middle_end PRIVATE
        RUNTIME_DIR="${CMAKE_CURRENT_SOURCE_DIR}/runtime"
    )

    target_link_libraries(compiler_middle_end Lib Yacc Opt Analysis)

    # 调试选项
    target_compile_options(compiler_middle_end PRIVATE -g -O0 -Wall -Wextra)

elseif(BUILD_AARCH64_BACKEND)
    message(STATUS "=== 构建完整编译器 (ARM64 后端) ===")
    set(CMAKE_CXX_CLANG_TIDY "") # 与其他分支一致，关闭代码检查以简化构建

    add_subdirectory(${PROJECT_SOURCE_DIR}/src/lib)
    add_subdirectory(${PROJECT_SOURCE_DIR}/src/yacc)
    add_subdirectory(${PROJECT_SOURCE_DIR}/src/ir)
    add_subdirectory(${PROJECT_SOURCE_DIR}/src/aarch64)

    add_executable(compiler main.cpp)
    target_compile_definitions(compiler PRIVATE
        RUNTIME_DIR="${CMAKE_CURRENT_SOURCE_DIR}/runtime"
        ENABLE_AARCH64_BACKEND=1
    )
    target_link_libraries(compiler Lib Yacc Opt Analysis BackendA64)

else()
    message(STATUS "=== 构建完整编译器 ===")
    set(CMAKE_CXX_CLANG_TIDY "") # 与其他分支一致，关闭代码检查以简化构建

    # 完整编译器构建
    add_subdirectory(${PROJECT_SOURCE_DIR}/src/lib)
    add_subdirectory(${PROJECT_SOURCE_DIR}/src/yacc)
    add_subdirectory(${PROJECT_SOURCE_DIR}/src/ir)
    add_subdirectory(${PROJECT_SOURCE_DIR}/src/riscv)

    add_executable(compiler main.cpp)
    target_compile_definitions(compiler PRIVATE
        RUNTIME_DIR="${CMAKE_CURRENT_SOURCE_DIR}/runtime"
        ENABLE_RISCV_BACKEND=1
    )
    target_link_libraries(compiler Lib Yacc Opt Analysis Backend)
endif()
