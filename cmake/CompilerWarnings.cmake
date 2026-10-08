# Aplica um conjunto consistente de avisos para MSVC, GCC e Clang.
function(anyps2_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
        # getenv/fopen/strncpy são usados de forma segura; silencia C4996.
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
        if(ANYPS2_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
            -Wnon-virtual-dtor -Wold-style-cast -Wcast-align -Woverloaded-virtual
            -Wimplicit-fallthrough)
        # No GCC 13 com -O3, -Wnull-dereference dá falsos positivos dentro da
        # própria libstdc++ (vector::insert, streambuf); fica só no Clang.
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            target_compile_options(${target} PRIVATE -Wnull-dereference)
        endif()
        if(ANYPS2_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()
