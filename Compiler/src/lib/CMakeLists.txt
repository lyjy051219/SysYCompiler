file(GLOB LIB_SRC CONFIGURE_DEPENDS "*.cpp")
add_library(Lib ${LIB_SRC})
target_include_directories(Lib PUBLIC ${PROJECT_SOURCE_DIR}/include/lib)
target_include_directories(Lib PUBLIC ${PROJECT_SOURCE_DIR}/include)
