# Copyright (c) 2026 Compiler contributors
# SPDX-License-Identifier: MIT

# 机器级优化 pass 归入 Backend 库，避免与 codegen 形成循环依赖。
file(GLOB BACKEND_OPT_SRC CONFIGURE_DEPENDS "*.cpp")
target_sources(Backend PRIVATE ${BACKEND_OPT_SRC})
 