file(GLOB LIB_SRC "*.cpp")
add_library(Lib ${LIB_SRC})
target_include_directories(Lib PUBLIC ${PROJECT_SOURCE_DIR}/include/lib)