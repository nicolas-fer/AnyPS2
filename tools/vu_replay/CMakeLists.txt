# Replay de gravações do VU1 contra o microcódigo recompilado de um jogo (ver main.cpp).
# Só entra no build com ANYPS2_VU_REPLAY_PROGRAMS (pasta dos vu_*.cpp gerados).
file(GLOB _vu_replay_programs "${ANYPS2_VU_REPLAY_PROGRAMS}/vu_*.cpp")
if(NOT _vu_replay_programs)
    message(FATAL_ERROR "ANYPS2_VU_REPLAY_PROGRAMS=${ANYPS2_VU_REPLAY_PROGRAMS} não contém vu_*.cpp (vu_000.cpp e vu_programs.cpp)")
endif()

add_executable(vu_replay main.cpp ${_vu_replay_programs})
target_link_libraries(vu_replay PRIVATE anyps2::runtime)
# Os arquivos gerados não seguem o nível de avisos do repositório; só o main.cpp segue.
anyps2_set_warnings(vu_replay)
set_source_files_properties(${_vu_replay_programs} PROPERTIES COMPILE_OPTIONS
    "$<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-O1;-w>;$<$<CXX_COMPILER_ID:MSVC>:/W0>")
