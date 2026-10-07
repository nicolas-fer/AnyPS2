# Incluído pelos projetos gerados por "anyps2 recomp". Traz o runtime do
# AnyPS2 (e o common) e define anyps2_generated_target().
#
#   set(ANYPS2_ROOT /caminho/para/AnyPS2)
#   include(${ANYPS2_ROOT}/cmake/AnyPS2Runtime.cmake)
#   add_executable(jogo ...)
#   anyps2_generated_target(jogo ${CMAKE_CURRENT_SOURCE_DIR}/jogo.image)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Tipo de build" FORCE)
endif()

if(NOT TARGET anyps2_common)
    add_subdirectory(${ANYPS2_ROOT}/common ${CMAKE_BINARY_DIR}/anyps2/common)
endif()
if(NOT TARGET anyps2_runtime)
    add_subdirectory(${ANYPS2_ROOT}/runtime ${CMAKE_BINARY_DIR}/anyps2/runtime)
endif()

function(anyps2_generated_target target image)
    target_link_libraries(${target} PRIVATE anyps2::runtime)
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    if(MSVC)
        # Funções geradas são grandes e cheias de rótulos.
        target_compile_options(${target} PRIVATE /utf-8 /bigobj /wd4102)
        # Código recompilado pode recursar fundo; a thread principal do guest já
        # roda numa thread de 64 MB, mas o main() do host também ganha folga.
        target_link_options(${target} PRIVATE /STACK:16777216)
    else()
        target_compile_options(${target} PRIVATE -Wno-unused-label -fno-strict-aliasing)
    endif()
    # A imagem com os segmentos do ELF fica ao lado do executável.
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${image} $<TARGET_FILE_DIR:${target}>
        VERBATIM)
endfunction()
