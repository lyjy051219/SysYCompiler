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
