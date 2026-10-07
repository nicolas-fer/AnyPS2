add_executable(anyps2_tests
    framework/minitest.cpp
    unit/test_elf.cpp
    unit/test_decoder.cpp
    unit/test_disasm.cpp
    unit/test_golden.cpp
    unit/test_runtime_ops.cpp
    unit/test_codegen.cpp
    unit/test_timing.cpp
)
target_include_directories(anyps2_tests PRIVATE framework unit)
target_link_libraries(anyps2_tests PRIVATE anyps2::recompiler anyps2::runtime)
target_compile_definitions(anyps2_tests PRIVATE
    ANYPS2_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
    ANYPS2_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
anyps2_set_warnings(anyps2_tests)
if(MSVC)
    target_compile_definitions(anyps2_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# Uma entrada do CTest por suíte.
foreach(suite elf decoder decoder_coverage disasm golden fixture runtime_ops codegen timing)
    add_test(NAME ${suite} COMMAND anyps2_tests ${suite})
endforeach()

# Teste de fumaça da CLI.
add_test(NAME cli_version COMMAND anyps2 --version)
add_test(NAME cli_info_fixture COMMAND anyps2 info ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/hello_r5900.elf)
add_test(NAME cli_disasm_fixture COMMAND anyps2 disasm ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/hello_r5900.elf)
add_test(NAME cli_recomp_missing_output COMMAND anyps2 recomp x)
set_tests_properties(cli_recomp_missing_output PROPERTIES WILL_FAIL TRUE)

# ---------------------------------------------------------------------------
# Ponta a ponta: homebrews compilados com o ps2dev (ELFs versionados em
# tests/homebrew; regenere com tests/homebrew/build.sh) são recompilados,
# compilados e executados. Desative com -DANYPS2_E2E_TESTS=OFF.
# ---------------------------------------------------------------------------
option(ANYPS2_E2E_TESTS "Testes de ponta a ponta com homebrews recompilados" ON)
if(ANYPS2_E2E_TESTS)
    enable_language(C)
    # Oráculo do cputest: o mesmo main.c compilado para o host.
    add_executable(cputest_host homebrew/cputest/main.c)
    if(MSVC)
        target_compile_definitions(cputest_host PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()

    if(CMAKE_CONFIGURATION_TYPES)
        set(e2e_build_type Release)
    else()
        set(e2e_build_type ${CMAKE_BUILD_TYPE})
    endif()

    function(anyps2_e2e name)
        cmake_parse_arguments(E "" "EXPECTED;EXPECTED_COMMAND;NAME;CLOCK" "ARGS" ${ARGN})
        if(NOT E_NAME)
            set(E_NAME ${name})
        endif()
        if(NOT E_CLOCK)
            set(E_CLOCK virtual)
        endif()
        set(work ${CMAKE_CURRENT_BINARY_DIR}/e2e/${E_NAME})
        string(REPLACE ";" "|" args "${E_ARGS}")
        set(cmd ${CMAKE_COMMAND}
            -DANYPS2=$<TARGET_FILE:anyps2>
            -DELF=${CMAKE_CURRENT_SOURCE_DIR}/homebrew/${name}/${name}.elf
            -DNAME=${name}
            -DWORK=${work}
            -DANYPS2_ROOT=${PROJECT_SOURCE_DIR}
            "-DGENERATOR=${CMAKE_GENERATOR}"
            -DBUILD_TYPE=${e2e_build_type}
            -DCLOCK=${E_CLOCK}
            -DCXX_COMPILER=${CMAKE_CXX_COMPILER}
            "-DARGS=${args}")
        if(E_EXPECTED_COMMAND)
            list(APPEND cmd -DEXPECTED_COMMAND=${E_EXPECTED_COMMAND})
        else()
            list(APPEND cmd -DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/homebrew/${name}/expected.txt)
        endif()
        list(APPEND cmd -P ${CMAKE_CURRENT_SOURCE_DIR}/e2e/run_homebrew.cmake)
        add_test(NAME e2e_${E_NAME} COMMAND ${cmd})
        set_tests_properties(e2e_${E_NAME} PROPERTIES TIMEOUT 900 LABELS e2e)
    endfunction()

    anyps2_e2e(hello ARGS a b)
    anyps2_e2e(cputest ARGS x y EXPECTED_COMMAND $<TARGET_FILE:cputest_host>)
    anyps2_e2e(threads)
    anyps2_e2e(fileio)
    anyps2_e2e(timers)
    # O mesmo teste no relógio real (tempo do host): a saída depende só da
    # ordem dos eventos, então também precisa bater.
    anyps2_e2e(timers NAME timers_real CLOCK real)
endif()
