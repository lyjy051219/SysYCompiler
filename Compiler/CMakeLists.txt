# Copyright (c) 2026 Compiler contributors
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.13)
project(compiler LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
 
# === 构建选项 ===
#如果需要使用哪个版本进行编译就去找相关的指令即可
option(BUILD_MIDDLE_END "构建中端(IR)用于调试" OFF)
option(BUILD_MIDDLE_END_RELEASE "构建中端(IR)的Release版本(-O2)" OFF)
option(BUILD_RISCV_BACKEND_RELEASE "构建RISC-V后端的Release版本(-O2)" OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(BUILD_MIDDLE_END_RELEASE)
    message(STATUS "=== 构建中端Release版本 (-O2) ===")

    add_subdirectory(${PROJECT_SOURCE_DIR}/src/lib)
    add_subdirectory(${PROJECT_SOURCE_DIR}/src/yacc)
    add_subdirectory(${PROJECT_SOURCE_DIR}/src/ir)

    add_executable(compiler_middle_end_release ${CMAKE_SOURCE_DIR}/main.cpp)
    target_compile_definitions(compiler_middle_end_release PRIVATE
        RUNTIME_DIR="${CMAKE_CURRENT_SOURCE_DIR}/runtime"
    )

    target_link_libraries(compiler_middle_end_release Lib Yacc Opt Analysis)
 
    # 保留调试符号，便于调试经过优化的代码。
    target_compile_options(compiler_middle_end_release PRIVATE
        -O2
        -g
        -Wall
        -Wextra
    )
elseif(BUILD_MIDDLE_END)
    message(STATUS "=== 构建中端调试版本 ===")

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

elseif(BUILD_RISCV_BACKEND_RELEASE)
    message(STATUS "=== 构建完整编译器 (RISC-V 后端 Release，-O2) ===")

    add_subdirectory(${PROJECT_SOURCE_DIR}/src/lib)
    add_subdirectory(${PROJECT_SOURCE_DIR}/src/yacc)
    add_subdirectory(${PROJECT_SOURCE_DIR}/src/ir)
    add_subdirectory(${PROJECT_SOURCE_DIR}/src/riscv)

    add_executable(compiler_riscv_release main.cpp)
    set_target_properties(compiler_riscv_release PROPERTIES OUTPUT_NAME compiler)
    target_compile_definitions(compiler_riscv_release PRIVATE
        RUNTIME_DIR="${CMAKE_CURRENT_SOURCE_DIR}/runtime"
    )
    target_link_libraries(compiler_riscv_release Lib Yacc Opt Analysis Backend)

    target_compile_options(compiler_riscv_release PRIVATE
        -O2
        -g
        -Wall
        -Wextra
    )

else()
    message(STATUS "=== 构建完整编译器 (RISC-V 后端) ===")

    # 完整编译器构建
    add_subdirectory(${PROJECT_SOURCE_DIR}/src/lib)
    add_subdirectory(${PROJECT_SOURCE_DIR}/src/yacc)
    add_subdirectory(${PROJECT_SOURCE_DIR}/src/ir)
    add_subdirectory(${PROJECT_SOURCE_DIR}/src/riscv)

    add_executable(compiler main.cpp)
    target_compile_definitions(compiler PRIVATE
        RUNTIME_DIR="${CMAKE_CURRENT_SOURCE_DIR}/runtime"
    )
    target_link_libraries(compiler Lib Yacc Opt Analysis Backend)
endif()
