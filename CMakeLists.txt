cmake_minimum_required(VERSION 3.20)

project(AnyPS2
    VERSION 0.1.0
    DESCRIPTION "Recompilador estático de PS2 (MIPS R5900) para PC"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Tipo de build" FORCE)
endif()

option(ANYPS2_BUILD_TESTS "Compila os testes automatizados" ON)
option(ANYPS2_WARNINGS_AS_ERRORS "Trata avisos do compilador como erros" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)

add_subdirectory(common)
add_subdirectory(recompiler)
add_subdirectory(runtime)
add_subdirectory(tools)

if(ANYPS2_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
