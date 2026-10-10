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
option(ANYPS2_VU_STATS "Contadores de diagnóstico do pipeline do VU (custam vazão; só para perfilar)" OFF)
# Dear ImGui (menu de configuração dentro da janela): com ANYPS2_IMGUI_DIR usa
# uma cópia local; senão baixa a versão fixada abaixo, se ANYPS2_FETCH_IMGUI.
# Só existe com ANYPS2_WITH_SDL=ON.
set(ANYPS2_IMGUI_DIR "" CACHE PATH "Pasta do Dear ImGui (vazio: baixar)")
option(ANYPS2_FETCH_IMGUI "Baixa o Dear ImGui se ANYPS2_IMGUI_DIR não for dado" ON)

# Formatos de disco e de módulos do IOP (ISO 9660, cabeçalho IRX, tabela de
# módulos com HLE), sem o resto do runtime: a CLI ("anyps2 disc") também usa.
add_library(anyps2_iopfmt STATIC
    src/iop/iso9660.cpp
    src/iop/irx.cpp
    src/iop/modules.cpp
)
add_library(anyps2::iopfmt ALIAS anyps2_iopfmt)
target_include_directories(anyps2_iopfmt PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(anyps2_iopfmt PUBLIC anyps2::common)
target_compile_features(anyps2_iopfmt PUBLIC cxx_std_20)
if(COMMAND anyps2_set_warnings)
    anyps2_set_warnings(anyps2_iopfmt)
endif()

add_library(anyps2_runtime STATIC
    src/memory.cpp
    src/ops.cpp
    src/runtime.cpp
    src/kernel.cpp
    src/kernel_threads.cpp
    src/host_thread.cpp
    src/host_profile.cpp
    src/hardware.cpp
    src/ipu.cpp
    src/ipu_mpeg.cpp
    src/iop/iop.cpp
    src/iop/loadfile.cpp
    src/iop/pad.cpp
    src/iop/dbcman.cpp
    src/iop/pdicdvd.cpp
    src/iop/lgdev.cpp
    src/iop/pdistr.cpp
    src/iop/pdispu2.cpp
    src/iop/mcserv.cpp
    src/iop/cdvd.cpp
    src/iop/spu2.cpp
    src/iop/audsrv.cpp
    src/audio.cpp
    src/input.cpp
    src/config.cpp
    src/timing.cpp
    src/dmac.cpp
    src/gif.cpp
    src/vif.cpp
    src/gs/vram.cpp
    src/gs/gs.cpp
    src/gs/gs_draw.cpp
    src/gs/gs_worker.cpp
    src/gs/gs_coverage.cpp
    src/gs/gs_bands.cpp
    src/gs/gs_trace.cpp
    src/video.cpp
    src/vu/vu.cpp
    src/vu/vu_trace.cpp
)
add_library(anyps2::runtime ALIAS anyps2_runtime)
target_include_directories(anyps2_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(anyps2_runtime PUBLIC anyps2::common anyps2::vu anyps2::iopfmt)
target_compile_features(anyps2_runtime PUBLIC cxx_std_20)
if(ANYPS2_VU_STATS)
    # PUBLIC: o C++ recompilado do VU inclui vu_exec.h e tem de ver o mesmo layout de Vu.
    target_compile_definitions(anyps2_runtime PUBLIC ANYPS2_VU_STATS=1)
endif()
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
                URL https://github.com/libsdl-org/SDL/releases/download/release-2.30.9/SDL2-2.30.9.tar.gz
                URL_HASH SHA256=24b574f71c87a763f50704bbb630cbe38298d544a1f890f099a4696b1d6beba4)
            FetchContent_MakeAvailable(anyps2_sdl2)
        else()
            message(FATAL_ERROR
                "SDL2 não encontrado. Instale-o (Linux: libsdl2-dev; Windows: vcpkg install sdl2), "
                "ou configure com -DANYPS2_FETCH_SDL=ON para baixá-lo, ou -DANYPS2_WITH_SDL=OFF para "
                "compilar só o modo sem janela.")
        endif()
    endif()
    set_property(GLOBAL PROPERTY ANYPS2_SDL_ORIGIN ${_anyps2_sdl_origin})
    # Os nomes do arquivo de configuração são conferidos com o SDL (config.h).
    target_compile_definitions(anyps2_runtime PUBLIC ANYPS2_SDL_NAMES=1)
    target_sources(anyps2_runtime PRIVATE src/video_sdl.cpp src/audio_sdl.cpp src/video_menu.cpp)
    if(TARGET SDL2::SDL2-static)
        set(_anyps2_sdl_target SDL2::SDL2-static)
    else()
        set(_anyps2_sdl_target SDL2::SDL2)
    endif()
    target_link_libraries(anyps2_runtime PRIVATE ${_anyps2_sdl_target})

    # Dear ImGui sem warnings próprios: é código de terceiros, fora de anyps2_set_warnings.
    if(ANYPS2_IMGUI_DIR)
        set(_anyps2_imgui_src ${ANYPS2_IMGUI_DIR})
    elseif(ANYPS2_FETCH_IMGUI)
        include(FetchContent)
        FetchContent_Declare(anyps2_imgui
            URL https://github.com/ocornut/imgui/archive/refs/tags/v1.90.9.tar.gz
            URL_HASH SHA256=04943919721e874ac75a2f45e6eb6c0224395034667bf508923388afda5a50bf)
        FetchContent_MakeAvailable(anyps2_imgui)
        set(_anyps2_imgui_src ${anyps2_imgui_SOURCE_DIR})
    else()
        message(FATAL_ERROR
            "Dear ImGui não informado: use -DANYPS2_IMGUI_DIR=<pasta>, ou -DANYPS2_FETCH_IMGUI=ON para "
            "baixá-lo, ou -DANYPS2_WITH_SDL=OFF para compilar só o modo sem janela.")
    endif()
    add_library(anyps2_imgui STATIC
        ${_anyps2_imgui_src}/imgui.cpp
        ${_anyps2_imgui_src}/imgui_draw.cpp
        ${_anyps2_imgui_src}/imgui_tables.cpp
        ${_anyps2_imgui_src}/imgui_widgets.cpp
        ${_anyps2_imgui_src}/backends/imgui_impl_sdl2.cpp
        ${_anyps2_imgui_src}/backends/imgui_impl_sdlrenderer2.cpp)
    # SYSTEM: os avisos de estilo (-Wold-style-cast etc.) são do ImGui, não nossos.
    target_include_directories(anyps2_imgui SYSTEM PUBLIC ${_anyps2_imgui_src} ${_anyps2_imgui_src}/backends)
    target_link_libraries(anyps2_imgui PUBLIC ${_anyps2_sdl_target})
    target_link_libraries(anyps2_runtime PRIVATE anyps2_imgui)
else()
    set_property(GLOBAL PROPERTY ANYPS2_SDL_ORIGIN none)
    target_sources(anyps2_runtime PRIVATE src/video_nosdl.cpp src/audio_nosdl.cpp)
endif()
