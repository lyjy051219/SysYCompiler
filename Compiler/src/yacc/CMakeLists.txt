file(GLOB YACC_SRC "*.cpp")
add_library(Yacc ${YACC_SRC})
target_link_libraries(Yacc Lib) #Yacc库依赖Lib库构建
target_include_directories(Yacc PUBLIC ${PROJECT_SOURCE_DIR}/include/yacc) #同时依赖自己的头文件
target_include_directories(Yacc PUBLIC ${PROJECT_SOURCE_DIR}/include)
