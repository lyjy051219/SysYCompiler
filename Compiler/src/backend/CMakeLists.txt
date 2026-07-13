file(GLOB BACKEND_SRC CONFIGURE_DEPENDS
    "*.cpp"
    "rv/*.cpp"
    "rv/opt/*.cpp"
)

add_library(Backend ${BACKEND_SRC})
target_link_libraries(Backend PUBLIC Lib Analysis)
target_include_directories(Backend PUBLIC
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/include/backend
    ${PROJECT_SOURCE_DIR}/include/riscv
)

option(BUILD_BACKEND_TESTS "构建后端单元测试" OFF)
if(BUILD_BACKEND_TESTS)
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/RegAllocTests.cpp)
        add_executable(regalloc_tests tests/RegAllocTests.cpp)
        target_link_libraries(regalloc_tests PRIVATE Backend)
        add_test(NAME regalloc_tests COMMAND regalloc_tests)
    endif()
    add_executable(block_placement_tests rv/tests/BlockPlacementTests.cpp)
    target_link_libraries(block_placement_tests PRIVATE Backend)
    add_test(NAME block_placement_tests COMMAND block_placement_tests)
    set_tests_properties(block_placement_tests PROPERTIES
        ENVIRONMENT "BACKEND_VERIFY_MIR=1")

    add_executable(pre_ra_machine_dce_tests rv/tests/PreRAMachineDCETests.cpp)
    target_link_libraries(pre_ra_machine_dce_tests PRIVATE Backend)
    add_test(NAME pre_ra_machine_dce_tests COMMAND pre_ra_machine_dce_tests)
    set_tests_properties(pre_ra_machine_dce_tests PROPERTIES
        ENVIRONMENT "BACKEND_VERIFY_MIR=1")

    add_executable(codegen_prepare_tests rv/tests/CodeGenPrepareTests.cpp)
    target_link_libraries(codegen_prepare_tests PRIVATE Backend)
    add_test(NAME codegen_prepare_tests COMMAND codegen_prepare_tests)

    add_executable(post_ra_list_schedule_tests rv/tests/PostRAListScheduleTests.cpp)
    target_link_libraries(post_ra_list_schedule_tests PRIVATE Backend)
    add_test(NAME post_ra_list_schedule_tests COMMAND post_ra_list_schedule_tests)
endif()
