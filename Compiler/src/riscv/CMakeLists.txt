# Copyright (c) 2026 Compiler contributors
# SPDX-License-Identifier: MIT

# RISC-V 后端：顶层 codegen 源文件构成 Backend 库，opt 子目录追加机器级 pass。
file(GLOB BACKEND_SRC CONFIGURE_DEPENDS "*.cpp")
add_library(Backend ${BACKEND_SRC})
target_link_libraries(Backend PUBLIC Lib Analysis)
target_include_directories(Backend PUBLIC ${PROJECT_SOURCE_DIR}/include)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/opt)
 