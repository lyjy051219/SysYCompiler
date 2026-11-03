# Copyright (c) 2026 Compiler contributors
# SPDX-License-Identifier: MIT

file(GLOB OPT_SRC CONFIGURE_DEPENDS "*.cpp")
add_library(Opt ${OPT_SRC})

target_link_libraries(Opt PUBLIC Lib Analysis)


# 使用 CMAKE_SOURCE_DIR 确保指向绝对根目录
target_include_directories(Opt PUBLIC
    "${CMAKE_SOURCE_DIR}/include"
) 
