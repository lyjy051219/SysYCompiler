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

    add_executable(pre_ra_affine_combine_tests
        rv/tests/PreRAAffineCombineTests.cpp)
    target_link_libraries(pre_ra_affine_combine_tests PRIVATE Backend)
    add_test(NAME pre_ra_affine_combine_tests
        COMMAND pre_ra_affine_combine_tests)
    set_tests_properties(pre_ra_affine_combine_tests PROPERTIES
        ENVIRONMENT "BACKEND_VERIFY_MIR=1")

    add_executable(pre_ra_guarded_loop_preheader_tests
        rv/tests/PreRAGuardedLoopPreheaderTests.cpp)
    target_link_libraries(pre_ra_guarded_loop_preheader_tests PRIVATE Backend)
    add_test(NAME pre_ra_guarded_loop_preheader_tests
        COMMAND pre_ra_guarded_loop_preheader_tests)
    set_tests_properties(pre_ra_guarded_loop_preheader_tests PROPERTIES
        ENVIRONMENT "BACKEND_VERIFY_MIR=1")

    add_executable(select_lowering_tests rv/tests/SelectLoweringTests.cpp)
    target_link_libraries(select_lowering_tests PRIVATE Backend)
    add_test(NAME select_lowering_tests COMMAND select_lowering_tests)
    set_tests_properties(select_lowering_tests PROPERTIES
        ENVIRONMENT "BACKEND_VERIFY_MIR=1")

    add_executable(isel_integer_lowering_tests
        rv/tests/ISelIntegerLoweringTests.cpp)
    target_link_libraries(isel_integer_lowering_tests PRIVATE Backend)
    add_test(NAME isel_integer_lowering_tests
        COMMAND isel_integer_lowering_tests)
    set_tests_properties(isel_integer_lowering_tests PROPERTIES
        ENVIRONMENT "BACKEND_VERIFY_MIR=1")

    add_executable(branch_compare_folding_tests
        rv/tests/BranchCompareFoldingTests.cpp)
    target_link_libraries(branch_compare_folding_tests PRIVATE Backend)
    add_test(NAME branch_compare_folding_tests
        COMMAND branch_compare_folding_tests)
    set_tests_properties(branch_compare_folding_tests PROPERTIES
        ENVIRONMENT "BACKEND_VERIFY_MIR=1")

    add_executable(phi_elimination_tests
        rv/tests/PhiEliminationTests.cpp)
    target_link_libraries(phi_elimination_tests PRIVATE Backend)
    add_test(NAME phi_elimination_tests COMMAND phi_elimination_tests)
    set_tests_properties(phi_elimination_tests PROPERTIES
        ENVIRONMENT "BACKEND_VERIFY_MIR=1")

    add_executable(codegen_prepare_tests rv/tests/CodeGenPrepareTests.cpp)
    target_link_libraries(codegen_prepare_tests PRIVATE Backend)
    add_test(NAME codegen_prepare_tests COMMAND codegen_prepare_tests)

    add_executable(post_ra_list_schedule_tests rv/tests/PostRAListScheduleTests.cpp)
    target_link_libraries(post_ra_list_schedule_tests PRIVATE Backend)
    add_test(NAME post_ra_list_schedule_tests COMMAND post_ra_list_schedule_tests)

    add_executable(post_ra_loop_backedge_bypass_tests
        rv/tests/PostRALoopBackedgeBypassTests.cpp)
    target_link_libraries(post_ra_loop_backedge_bypass_tests PRIVATE Backend)
    add_test(NAME post_ra_loop_backedge_bypass_tests
        COMMAND post_ra_loop_backedge_bypass_tests)
    set_tests_properties(post_ra_loop_backedge_bypass_tests PROPERTIES
        ENVIRONMENT "BACKEND_VERIFY_MIR=1")
endif()
