# Runtime que o código gerado linka (CPU, memória, syscalls HLE, hardware).
# Começa na Fase 2; por enquanto só expõe os headers comuns.
add_library(anyps2_runtime INTERFACE)
add_library(anyps2::runtime ALIAS anyps2_runtime)
target_link_libraries(anyps2_runtime INTERFACE anyps2::common)
