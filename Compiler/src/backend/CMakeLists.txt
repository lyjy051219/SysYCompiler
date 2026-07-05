file(GLOB BACKEND_SRC CONFIGURE_DEPENDS
    "*.cpp"
    "rv/*.cpp"
)

add_library(Backend ${BACKEND_SRC})
target_link_libraries(Backend PUBLIC Lib)
target_include_directories(Backend PUBLIC
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/include/backend
    ${PROJECT_SOURCE_DIR}/include/riscv
)

option(BUILD_BACKEND_TESTS "构建后端单元测试" OFF)
if(BUILD_BACKEND_TESTS)
    enable_testing()
    add_executable(regalloc_tests tests/RegAllocTests.cpp)
    target_link_libraries(regalloc_tests PRIVATE Backend)
    add_test(NAME regalloc_tests COMMAND regalloc_tests)
endif()
