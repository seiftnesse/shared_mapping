add_executable(sm_tests)

nice_target_sources(sm_tests ${SF}
    PRIVATE
    tests/test_pml4.cpp
    tests/test_paging_entry.cpp
    tests/test_kernel_offsets.cpp
)

target_include_directories(sm_tests PRIVATE ${SF})
target_link_libraries(sm_tests PRIVATE gtest_main)
set_target_properties(sm_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

add_test(NAME sm_unit_tests COMMAND sm_tests)
