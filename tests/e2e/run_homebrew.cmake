# Teste de ponta a ponta de um homebrew:
#   1. anyps2 recomp <ELF> -o <WORK>/gen
#   2. configura e compila o projeto gerado (mesmo gerador/compilador do build)
#   3. executa e compara a saída com o esperado
#
# Variáveis: ANYPS2, ELF, NAME, WORK, ANYPS2_ROOT, GENERATOR, BUILD_TYPE,
#            CXX_COMPILER, CXX_FLAGS, LINKER_FLAGS, WITH_SDL (opcionais),
#            EXPECTED (arquivo) ou EXPECTED_COMMAND
#            (executável cuja saída é o esperado), ARGS (lista), HOST_DIR,
#            CLOCK (virtual|real; padrão virtual), EXPECTED_FRAME (PNG que a
#            última imagem exibida deve reproduzir byte a byte).

string(REPLACE "|" ";" ARGS "${ARGS}")

function(run_step)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "falhou (${rc}): ${ARGN}\n${out}\n${err}")
    endif()
endfunction()

file(REMOVE_RECURSE ${WORK}/gen/src)
run_step(${ANYPS2} recomp ${ELF} -o ${WORK}/gen --name ${NAME} --root ${ANYPS2_ROOT})

set(configure_args -S ${WORK}/gen -B ${WORK}/build -G ${GENERATOR})
if(BUILD_TYPE)
    list(APPEND configure_args -DCMAKE_BUILD_TYPE=${BUILD_TYPE})
endif()
if(CXX_COMPILER)
    list(APPEND configure_args -DCMAKE_CXX_COMPILER=${CXX_COMPILER})
endif()
# Mesmas flags do build principal (ex.: sanitizers também no código gerado).
if(CXX_FLAGS)
    list(APPEND configure_args "-DCMAKE_CXX_FLAGS=${CXX_FLAGS}")
endif()
if(LINKER_FLAGS)
    list(APPEND configure_args "-DCMAKE_EXE_LINKER_FLAGS=${LINKER_FLAGS}")
endif()
if(DEFINED WITH_SDL)
    list(APPEND configure_args -DANYPS2_WITH_SDL=${WITH_SDL})
endif()
run_step(${CMAKE_COMMAND} ${configure_args})
run_step(${CMAKE_COMMAND} --build ${WORK}/build --config ${BUILD_TYPE})

# Localiza o executável (geradores multi-config usam um subdiretório).
set(exe "")
foreach(candidate ${WORK}/build/${BUILD_TYPE}/${NAME}${CMAKE_EXECUTABLE_SUFFIX}
                  ${WORK}/build/${NAME}${CMAKE_EXECUTABLE_SUFFIX}
                  ${WORK}/build/${NAME}.exe ${WORK}/build/${BUILD_TYPE}/${NAME}.exe ${WORK}/build/${NAME})
    if(EXISTS ${candidate} AND NOT IS_DIRECTORY ${candidate})
        set(exe ${candidate})
        break()
    endif()
endforeach()
if(NOT exe)
    message(FATAL_ERROR "executável ${NAME} não encontrado em ${WORK}/build")
endif()

if(NOT HOST_DIR)
    set(HOST_DIR ${WORK}/host)
endif()
file(REMOVE_RECURSE ${HOST_DIR})
file(MAKE_DIRECTORY ${HOST_DIR})
set(ENV{ANYPS2_HOST_DIR} ${HOST_DIR})
# Relógio virtual por padrão: execução determinística.
if(NOT CLOCK)
    set(CLOCK virtual)
endif()
set(ENV{ANYPS2_CLOCK} ${CLOCK})
set(ENV{ANYPS2_VIDEO} none)
if(EXPECTED_FRAME)
    file(REMOVE ${WORK}/frame.png)
    set(ENV{ANYPS2_SCREENSHOT} ${WORK}/frame.png)
endif()
execute_process(COMMAND ${exe} ${ARGS} WORKING_DIRECTORY ${HOST_DIR}
                RESULT_VARIABLE rc OUTPUT_VARIABLE actual ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${NAME} terminou com código ${rc}\n--- stdout ---\n${actual}\n--- stderr ---\n${err}")
endif()

if(EXPECTED_COMMAND)
    execute_process(COMMAND ${EXPECTED_COMMAND} ${ARGS} RESULT_VARIABLE rc OUTPUT_VARIABLE expected)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "oráculo ${EXPECTED_COMMAND} falhou (${rc})")
    endif()
else()
    file(READ ${EXPECTED} expected)
endif()
string(REPLACE "\r\n" "\n" actual "${actual}")
string(REPLACE "\r\n" "\n" expected "${expected}")
if(NOT actual STREQUAL expected)
    file(WRITE ${WORK}/actual.txt "${actual}")
    file(WRITE ${WORK}/expected.txt "${expected}")
    message(FATAL_ERROR "saída de ${NAME} difere do esperado\n--- obtido ---\n${actual}\n--- esperado ---\n${expected}\n(arquivos em ${WORK})")
endif()
message(STATUS "${NAME}: saída idêntica ao esperado")

if(EXPECTED_FRAME)
    if(NOT EXISTS ${WORK}/frame.png)
        message(FATAL_ERROR "${NAME}: nenhuma imagem foi exibida (esperava ${EXPECTED_FRAME})")
    endif()
    execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files ${WORK}/frame.png ${EXPECTED_FRAME}
                    RESULT_VARIABLE differ)
    if(differ)
        message(FATAL_ERROR "${NAME}: a imagem exibida difere da referência\n"
                            "  obtida:   ${WORK}/frame.png\n  esperada: ${EXPECTED_FRAME}")
    endif()
    message(STATUS "${NAME}: imagem idêntica à referência")
endif()
