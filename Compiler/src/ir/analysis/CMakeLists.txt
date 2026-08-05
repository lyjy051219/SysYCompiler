file(GLOB_RECURSE ANALYSIS_SRC CONFIGURE_DEPENDS "*.cpp")
add_library(Analysis ${ANALYSIS_SRC})
target_link_libraries(Analysis Lib)
target_include_directories(Analysis PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)
