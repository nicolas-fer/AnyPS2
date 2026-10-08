# Triagem de discos ("anyps2 disc"), separada para os testes também usarem.
add_library(anyps2_disc STATIC anyps2/disc.cpp)
target_include_directories(anyps2_disc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/anyps2)
target_link_libraries(anyps2_disc PUBLIC anyps2::recompiler anyps2::iopfmt)
anyps2_set_warnings(anyps2_disc)

add_executable(anyps2 anyps2/main.cpp)
target_link_libraries(anyps2 PRIVATE anyps2::recompiler anyps2_disc)
anyps2_set_warnings(anyps2)
target_compile_definitions(anyps2 PRIVATE ANYPS2_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
