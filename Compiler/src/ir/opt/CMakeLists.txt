file(GLOB OPT_SRC "*.cpp")
add_library(Opt ${OPT_SRC})

target_link_libraries(Opt PUBLIC Lib Analysis)

# === 调试信息：打印我们正在设置的路径 ===
message(WARNING ">>> DEBUG: CMAKE_SOURCE_DIR is: ${CMAKE_SOURCE_DIR}")
message(WARNING ">>> DEBUG: Setting Opt include dir to: ${CMAKE_SOURCE_DIR}/include")
# ========================================

# 使用 CMAKE_SOURCE_DIR 确保指向绝对根目录
target_include_directories(Opt PUBLIC
    "${CMAKE_SOURCE_DIR}/include"
)