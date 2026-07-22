# 机器级优化 pass 归入 BackendA64 库，避免与 codegen 形成循环依赖。
file(GLOB BACKEND_A64_OPT_SRC CONFIGURE_DEPENDS "*.cpp")
target_sources(BackendA64 PRIVATE ${BACKEND_A64_OPT_SRC})
