# Biblioteca do recompilador: parser de ELF, decodificador/disassembler do
# R5900 e (a partir da Fase 2) análise de funções e gerador de C++.
add_library(anyps2_recompiler STATIC
    src/analysis/analyzer.cpp
    src/codegen/generator.cpp
    src/elf/elf_file.cpp
    src/r5900/opcodes.cpp
    src/r5900/decoder.cpp
    src/r5900/disassembler.cpp
    src/r5900/registers.cpp
)
add_library(anyps2::recompiler ALIAS anyps2_recompiler)

target_include_directories(anyps2_recompiler PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(anyps2_recompiler PUBLIC anyps2::common)
target_compile_features(anyps2_recompiler PUBLIC cxx_std_20)
anyps2_set_warnings(anyps2_recompiler)
