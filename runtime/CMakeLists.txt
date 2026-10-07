# Runtime que o código gerado linka: contexto da CPU, memória, kernel do EE
# em HLE, hardware e IOP em HLE.
add_library(anyps2_runtime STATIC
    src/memory.cpp
    src/ops.cpp
    src/runtime.cpp
    src/kernel.cpp
    src/kernel_threads.cpp
    src/host_thread.cpp
    src/hardware.cpp
    src/iop.cpp
    src/timing.cpp
)
add_library(anyps2::runtime ALIAS anyps2_runtime)
target_include_directories(anyps2_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(anyps2_runtime PUBLIC anyps2::common)
target_compile_features(anyps2_runtime PUBLIC cxx_std_20)
find_package(Threads REQUIRED)
target_link_libraries(anyps2_runtime PUBLIC Threads::Threads)
if(COMMAND anyps2_set_warnings)
    anyps2_set_warnings(anyps2_runtime)
endif()
