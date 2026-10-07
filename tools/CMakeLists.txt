add_executable(anyps2 anyps2/main.cpp)
target_link_libraries(anyps2 PRIVATE anyps2::recompiler)
anyps2_set_warnings(anyps2)
