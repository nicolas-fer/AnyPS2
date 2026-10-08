# Runtime que o código gerado linka: contexto da CPU, memória, kernel do EE
# em HLE, hardware (DMAC, GIF, VIF, GS em software), IOP em HLE e a saída de
# vídeo (SDL2).
option(ANYPS2_WITH_SDL "Janela/entrada/áudio via SDL2 (OFF: só modo sem janela)" ON)
if(WIN32)
    set(_anyps2_fetch_default ON)
else()
    set(_anyps2_fetch_default OFF)
endif()
option(ANYPS2_FETCH_SDL "Baixa e compila o SDL2 se ele não for encontrado no sistema" ${_anyps2_fetch_default})

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
    src/dmac.cpp
    src/gif.cpp
    src/vif.cpp
    src/gs/vram.cpp
    src/gs/gs.cpp
    src/gs/gs_draw.cpp
    src/video.cpp
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
# Resultados de ponto flutuante iguais em qualquer CPU/compilador (sem FMA
# implícito): a FPU do guest e o rasterizador do GS dependem disso, e os
# testes comparam imagens byte a byte.
if(NOT MSVC)
    target_compile_options(anyps2_runtime PUBLIC -ffp-contract=off)
else()
    target_compile_options(anyps2_runtime PUBLIC /fp:precise)
endif()

if(ANYPS2_WITH_SDL)
    set(_anyps2_sdl_origin system)
    if(NOT TARGET SDL2::SDL2 AND NOT TARGET SDL2::SDL2-static)
        find_package(SDL2 CONFIG QUIET)
    endif()
    if(NOT TARGET SDL2::SDL2 AND NOT TARGET SDL2::SDL2-static)
        set(_anyps2_sdl_origin fetched)
        if(ANYPS2_FETCH_SDL)
            include(FetchContent)
            set(SDL_SHARED OFF CACHE BOOL "" FORCE)
            set(SDL_STATIC ON CACHE BOOL "" FORCE)
            set(SDL_TEST OFF CACHE BOOL "" FORCE)
            FetchContent_Declare(anyps2_sdl2
                URL https://github.com/libsdl-org/SDL/releases/download/release-2.30.9/SDL2-2.30.9.tar.gz)
            FetchContent_MakeAvailable(anyps2_sdl2)
        else()
            message(FATAL_ERROR
                "SDL2 não encontrado. Instale-o (Linux: libsdl2-dev; Windows: vcpkg install sdl2), "
                "ou configure com -DANYPS2_FETCH_SDL=ON para baixá-lo, ou -DANYPS2_WITH_SDL=OFF para "
                "compilar só o modo sem janela.")
        endif()
    endif()
    set_property(GLOBAL PROPERTY ANYPS2_SDL_ORIGIN ${_anyps2_sdl_origin})
    target_sources(anyps2_runtime PRIVATE src/video_sdl.cpp)
    if(TARGET SDL2::SDL2-static)
        target_link_libraries(anyps2_runtime PRIVATE SDL2::SDL2-static)
    else()
        target_link_libraries(anyps2_runtime PRIVATE SDL2::SDL2)
    endif()
else()
    set_property(GLOBAL PROPERTY ANYPS2_SDL_ORIGIN none)
    target_sources(anyps2_runtime PRIVATE src/video_nosdl.cpp)
endif()
