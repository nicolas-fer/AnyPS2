# Triagem de discos ("anyps2 disc"), separada para os testes também usarem.
add_library(anyps2_disc STATIC anyps2/disc.cpp)
target_include_directories(anyps2_disc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/anyps2)
target_link_libraries(anyps2_disc PUBLIC anyps2::recompiler anyps2::iopfmt)
anyps2_set_warnings(anyps2_disc)

add_executable(anyps2 anyps2/main.cpp)
target_link_libraries(anyps2 PRIVATE anyps2::recompiler anyps2_disc)
anyps2_set_warnings(anyps2)
target_compile_definitions(anyps2 PRIVATE ANYPS2_SOURCE_DIR="${PROJECT_SOURCE_DIR}")

# Reprodutor de GS dumps do PCSX2 no GS em software (a biblioteca também é dos testes).
add_library(anyps2_gsplay_lib STATIC gsplay/gsplay.cpp)
target_include_directories(anyps2_gsplay_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/gsplay)
target_link_libraries(anyps2_gsplay_lib PUBLIC anyps2::runtime)
anyps2_set_warnings(anyps2_gsplay_lib)

add_executable(anyps2_gsplay gsplay/main.cpp)
target_link_libraries(anyps2_gsplay PRIVATE anyps2_gsplay_lib)
anyps2_set_warnings(anyps2_gsplay)
