# Conjunto de instruções dos VUs (modo micro): decodificação e disassembler.
# Usado pelo runtime (interpretador) e pelo recompilador (microprogramas).
add_library(anyps2_vu STATIC
    src/decode.cpp
    src/disasm.cpp
    src/scan.cpp
)
add_library(anyps2::vu ALIAS anyps2_vu)
target_include_directories(anyps2_vu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(anyps2_vu PUBLIC cxx_std_20)
if(COMMAND anyps2_set_warnings)
    anyps2_set_warnings(anyps2_vu)
endif()
