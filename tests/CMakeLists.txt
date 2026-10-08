add_executable(anyps2_tests
    framework/minitest.cpp
    unit/test_elf.cpp
    unit/test_decoder.cpp
    unit/test_disasm.cpp
    unit/test_golden.cpp
    unit/test_runtime_ops.cpp
    unit/test_codegen.cpp
    unit/test_timing.cpp
    unit/test_gs.cpp
    unit/test_vu_isa.cpp
    unit/test_vu.cpp
    unit/test_iop.cpp
    unit/test_disc.cpp
)
target_include_directories(anyps2_tests PRIVATE framework unit)
target_link_libraries(anyps2_tests PRIVATE anyps2::recompiler anyps2::runtime anyps2::vu anyps2_disc)
target_compile_definitions(anyps2_tests PRIVATE
    ANYPS2_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
    ANYPS2_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
    ANYPS2_TEST_HOMEBREW_DIR="${CMAKE_CURRENT_SOURCE_DIR}/homebrew")
anyps2_set_warnings(anyps2_tests)
if(MSVC)
    target_compile_definitions(anyps2_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# Uma entrada do CTest por suíte.
foreach(suite elf decoder decoder_coverage disasm golden fixture runtime_ops codegen timing gs vu_isa vu iop disc)
    add_test(NAME ${suite} COMMAND anyps2_tests ${suite})
endforeach()

# Teste diferencial do microcódigo de VU recompilado: vu_fuzz_gen sorteia
# microprogramas a partir do golden do dvp-objdump e gera o C++ com o mesmo
# gerador do "anyps2 recomp"; anyps2_vu_diff compara com o interpretador.
add_executable(vu_fuzz_gen vudiff/gen.cpp)
target_link_libraries(vu_fuzz_gen PRIVATE anyps2::recompiler)
anyps2_set_warnings(vu_fuzz_gen)
set(vudiff_gen ${CMAKE_CURRENT_BINARY_DIR}/vudiff)
set(vudiff_sources ${vudiff_gen}/vu_fuzz_0.cpp ${vudiff_gen}/vu_fuzz_1.cpp ${vudiff_gen}/vu_fuzz_2.cpp
    ${vudiff_gen}/vu_fuzz_3.cpp)
add_custom_command(
    OUTPUT ${vudiff_gen}/vu_fuzz_data.h ${vudiff_sources}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${vudiff_gen}
    COMMAND vu_fuzz_gen ${CMAKE_CURRENT_SOURCE_DIR}/data/vu_golden.txt ${vudiff_gen}
    DEPENDS vu_fuzz_gen ${CMAKE_CURRENT_SOURCE_DIR}/data/vu_golden.txt
    COMMENT "Gerando microprogramas aleatórios do VU e o C++ recompilado")
add_executable(anyps2_vu_diff vudiff/main.cpp ${vudiff_sources})
target_include_directories(anyps2_vu_diff PRIVATE ${vudiff_gen})
# Mesmo nível de otimização que o "anyps2 recomp" usa no microcódigo gerado.
# Com UBSan, o C++ gerado (todo o núcleo do VU inline em cada par) leva mais
# de 10 min para compilar; esses arquivos ficam só com ASan — o mesmo núcleo
# continua sob UBSan pelo interpretador nos outros testes.
set(vudiff_options "$<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-O1>")
if(CMAKE_CXX_FLAGS MATCHES "sanitize=[^ ]*undefined")
    list(APPEND vudiff_options "$<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-fno-sanitize=undefined>")
endif()
set_source_files_properties(${vudiff_sources} PROPERTIES COMPILE_OPTIONS "${vudiff_options}")
target_link_libraries(anyps2_vu_diff PRIVATE anyps2::runtime)
anyps2_set_warnings(anyps2_vu_diff)
add_test(NAME vu_diff COMMAND anyps2_vu_diff)

# Teste de fumaça da CLI.
add_test(NAME cli_version COMMAND anyps2 --version)
add_test(NAME cli_info_fixture COMMAND anyps2 info ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/hello_r5900.elf)
add_test(NAME cli_disc COMMAND anyps2 disc ${CMAKE_CURRENT_SOURCE_DIR}/homebrew/cdvd/disc.iso)
# ram2elf: ELF a partir de uma "RAM" (aqui, os bytes de um ELF qualquer).
add_test(NAME cli_ram2elf COMMAND anyps2 ram2elf ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/hello_r5900.elf
         --entry 0x40 --text 0x0 0x80 --data 0x80 0x100 --bss-end 0x200 -o ${CMAKE_CURRENT_BINARY_DIR}/ram2elf.elf)
set_tests_properties(cli_ram2elf PROPERTIES PASS_REGULAR_EXPRESSION "ELF gerado: .*entrada 0x00000040, 2 segmento")
set_tests_properties(cli_disc PROPERTIES PASS_REGULAR_EXPRESSION "BOOT2 = cdrom0:.AP2_000.00;1")
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
    # Os testes rodam sem janela; os projetos gerados só usam o SDL se ele
    # vier do sistema (compilar um SDL baixado em cada projeto seria lento).
    get_property(e2e_sdl_origin GLOBAL PROPERTY ANYPS2_SDL_ORIGIN)
    if(e2e_sdl_origin STREQUAL "system")
        set(e2e_with_sdl ON)
    else()
        set(e2e_with_sdl OFF)
    endif()

    function(anyps2_e2e name)
        cmake_parse_arguments(E "" "EXPECTED;EXPECTED_COMMAND;NAME;CLOCK;EXPECT_FAIL" "ARGS;ENV;RECOMP_ARGS" ${ARGN})
        if(NOT E_NAME)
            set(E_NAME ${name})
        endif()
        if(NOT E_CLOCK)
            set(E_CLOCK virtual)
        endif()
        set(work ${CMAKE_CURRENT_BINARY_DIR}/e2e/${E_NAME})
        string(REPLACE ";" "|" args "${E_ARGS}")
        string(REPLACE ";" "|" env "${E_ENV}")
        string(REPLACE ";" "|" recomp_args "${E_RECOMP_ARGS}")
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
            "-DCXX_FLAGS=${CMAKE_CXX_FLAGS}"
            "-DLINKER_FLAGS=${CMAKE_EXE_LINKER_FLAGS}"
            -DWITH_SDL=${e2e_with_sdl}
            "-DARGS=${args}"
            "-DENV=${env}"
            "-DRECOMP_ARGS=${recomp_args}")
        if(E_EXPECTED_COMMAND)
            list(APPEND cmd -DEXPECTED_COMMAND=${E_EXPECTED_COMMAND})
        elseif(E_EXPECTED)
            list(APPEND cmd -DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/homebrew/${name}/${E_EXPECTED})
        else()
            list(APPEND cmd -DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/homebrew/${name}/expected.txt)
        endif()
        if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/homebrew/${name}/expected.png)
            list(APPEND cmd -DEXPECTED_FRAME=${CMAKE_CURRENT_SOURCE_DIR}/homebrew/${name}/expected.png)
        endif()
        if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/homebrew/${name}/expected.wav)
            list(APPEND cmd -DEXPECTED_WAV=${CMAKE_CURRENT_SOURCE_DIR}/homebrew/${name}/expected.wav)
        endif()
        if(E_EXPECT_FAIL)
            list(APPEND cmd "-DEXPECT_FAIL=${E_EXPECT_FAIL}")
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
    # No relógio real o tempo emulado anda com o do host: com outros testes
    # compilando em paralelo, a troca entre threads do guest pode levar mais
    # que as margens de 10 ms do teste. Roda sozinho.
    set_tests_properties(e2e_timers_real PROPERTIES RUN_SERIAL TRUE)
    # Fase 4: gráficos (saída de texto + imagem final comparada byte a byte)
    anyps2_e2e(gfx2d)
    anyps2_e2e(cube3d)
    anyps2_e2e(gskit)
    # Fase 5: VU0 (macro e micro) e VU1 (microprograma + XGKICK). Os samples
    # do ps2sdk rodam para sempre: ANYPS2_FRAMES encerra no N-ésimo VBlank.
    # ANYPS2_VU=compiled: todo par de microcódigo executado tem de vir do
    # código recompilado; vu1draw_interp roda o mesmo só no interpretador e
    # tem de dar a mesma saída e a mesma imagem.
    anyps2_e2e(vu0math ENV ANYPS2_VU=compiled)
    anyps2_e2e(vu1draw ENV ANYPS2_VU=compiled)
    anyps2_e2e(vu1draw NAME vu1draw_interp ENV ANYPS2_VU=interp)
    anyps2_e2e(sdk_cube ENV ANYPS2_FRAMES=60)
    anyps2_e2e(sdk_teapot ENV ANYPS2_FRAMES=8)
    anyps2_e2e(sdk_texture ENV ANYPS2_FRAMES=8)
    anyps2_e2e(sdk_vu1 ENV ANYPS2_FRAMES=8 ANYPS2_VU=compiled)
    # Fase 6: IOP em HLE. Módulos (rom0: e IRX embutido; sem HLE = erro
    # claro), controles com roteiro determinístico nos dois protocolos do
    # padman, memory card em pasta do host, disco a partir de uma ISO (e o
    # erro sem ISO) e som (WAV comparado byte a byte).
    anyps2_e2e(modules)
    anyps2_e2e(modules NAME modules_unknown ARGS unknown EXPECTED expected_unknown.txt
               EXPECT_FAIL "módulo IRX .USB_driver. v2.4 .* não tem implementação HLE")
    anyps2_e2e(modules NAME modules_rom ARGS rom EXPECTED expected_rom.txt
               EXPECT_FAIL "rom0:FOOBAR .ROM do console. não tem implementação HLE")
    anyps2_e2e(pad ENV ANYPS2_PAD_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/homebrew/pad/script.txt)
    anyps2_e2e(pad NAME pad_rom ARGS rom EXPECTED expected_rom.txt
               ENV ANYPS2_PAD_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/homebrew/pad/script.txt)
    anyps2_e2e(memcard)
    anyps2_e2e(cdvd ENV ANYPS2_ISO=${CMAKE_CURRENT_SOURCE_DIR}/homebrew/cdvd/disc.iso)
    anyps2_e2e(cdvd NAME cdvd_noiso EXPECTED expected_noiso.txt EXPECT_FAIL "nenhuma imagem foi informada")
    anyps2_e2e(audio)

    # Fase 7: o que jogos comerciais fazem no boot (tabela de syscalls do
    # kernel, ERET, fileio do SDK 3.0, reboot do IOP na ordem da Sony).
    anyps2_e2e(kpatch)
    # Boot que põe outro programa na memória e chama ExecPS2 (como o GT4): o
    # filho entra no projeto com --extra; sem ele, erro claro.
    anyps2_e2e(execps2 RECOMP_ARGS --extra ${CMAKE_CURRENT_SOURCE_DIR}/homebrew/execchild/execchild.elf)
    # Controles do SDK 3.0 (dbcman/libdbc/libpad2) e sceMcGetSlotMax.
    anyps2_e2e(dbcpad ENV ANYPS2_PAD_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/homebrew/dbcpad/script.txt)
    anyps2_e2e(execps2 NAME execps2_missing EXPECTED expected_missing.txt
               EXPECT_FAIL "ExecPS2.0x00100B20.*foi recompilado")
endif()
