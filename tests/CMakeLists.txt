add_executable(anyps2_tests
    framework/minitest.cpp
    unit/test_elf.cpp
    unit/test_decoder.cpp
    unit/test_disasm.cpp
    unit/test_golden.cpp
)
target_include_directories(anyps2_tests PRIVATE framework unit)
target_link_libraries(anyps2_tests PRIVATE anyps2::recompiler)
target_compile_definitions(anyps2_tests PRIVATE
    ANYPS2_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
    ANYPS2_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
anyps2_set_warnings(anyps2_tests)
if(MSVC)
    target_compile_definitions(anyps2_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# Uma entrada do CTest por suíte.
foreach(suite elf decoder decoder_coverage disasm golden fixture)
    add_test(NAME ${suite} COMMAND anyps2_tests ${suite})
endforeach()

# Teste de fumaça da CLI.
add_test(NAME cli_version COMMAND anyps2 --version)
add_test(NAME cli_info_fixture COMMAND anyps2 info ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/hello_r5900.elf)
add_test(NAME cli_disasm_fixture COMMAND anyps2 disasm ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/hello_r5900.elf)
add_test(NAME cli_recomp_not_yet COMMAND anyps2 recomp x)
set_tests_properties(cli_recomp_not_yet PROPERTIES WILL_FAIL TRUE)
