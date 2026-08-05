# ARM64(AArch64) 后端：顶层 codegen 源文件构成 BackendA64 库，opt 子目录追加机器级 pass。
file(GLOB BACKEND_A64_SRC CONFIGURE_DEPENDS "*.cpp")
add_library(BackendA64 ${BACKEND_A64_SRC})
target_link_libraries(BackendA64 PUBLIC Lib Analysis)
target_include_directories(BackendA64 PUBLIC ${PROJECT_SOURCE_DIR}/include)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/opt)
