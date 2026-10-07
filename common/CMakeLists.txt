# Utilitários compartilhados entre recompilador, runtime e ferramentas
# (header-only por enquanto).
add_library(anyps2_common INTERFACE)
add_library(anyps2::common ALIAS anyps2_common)
target_include_directories(anyps2_common INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(anyps2_common INTERFACE cxx_std_20)
