cmake_minimum_required(VERSION 3.20)

project(PwnieIslandResearch LANGUAGES C CXX)

if(NOT WIN32)
    message(FATAL_ERROR "This project is Windows-only.")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR
        "Use a 32-bit compiler. Pwn Adventure 3 Windows client is 32-bit.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    # v1.3.4, pinned to an immutable commit for reproducible rebuilds.
    GIT_TAG c3fcafdc10146beb5919319d0683e44e3c30d537
)

FetchContent_MakeAvailable(minhook)

add_executable(
    pwnie_external
    external_tool.cpp
)

target_compile_definitions(
    pwnie_external
    PRIVATE
        UNICODE
        _UNICODE
)

add_library(
    pwnie_research
    SHARED
    research_dll.cpp
)

target_compile_definitions(
    pwnie_research
    PRIVATE
        UNICODE
        _UNICODE
)

target_link_libraries(
    pwnie_research
    PRIVATE
        minhook
)

set_target_properties(
    pwnie_research
    PROPERTIES
        PREFIX ""
        OUTPUT_NAME "pwnie_research"
)

if(MINGW)
    target_link_options(
        pwnie_external
        PRIVATE
            -static
            -static-libgcc
            -static-libstdc++
    )

    target_link_options(
        pwnie_research
        PRIVATE
            -static
            -static-libgcc
            -static-libstdc++
    )
endif()
