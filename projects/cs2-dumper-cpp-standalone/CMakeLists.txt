cmake_minimum_required(VERSION 3.20)
project(cs2_dumper_cpp_standalone VERSION 1.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(cs2dumper_core
    src/analysis.cpp
    src/memory.cpp
    src/memory_win32.cpp
    src/output.cpp
    src/pattern.cpp
    src/pe.cpp
    src/runtime_config.cpp
)

target_include_directories(cs2dumper_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

if(MSVC)
    target_compile_options(cs2dumper_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(cs2dumper_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(cs2-dumper src/main.cpp)
target_link_libraries(cs2-dumper PRIVATE cs2dumper_core)

add_custom_command(TARGET cs2-dumper POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/config/signatures.cfg"
        "$<TARGET_FILE_DIR:cs2-dumper>/signatures.cfg"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/config/layout.cfg"
        "$<TARGET_FILE_DIR:cs2-dumper>/layout.cfg"
)

install(TARGETS cs2-dumper RUNTIME DESTINATION bin)
install(FILES config/signatures.cfg config/layout.cfg DESTINATION bin)

include(CTest)
if(BUILD_TESTING)
    add_executable(pattern_tests tests/pattern_tests.cpp)
    target_link_libraries(pattern_tests PRIVATE cs2dumper_core)
    add_test(NAME pattern_tests COMMAND pattern_tests)

    add_executable(output_tests tests/output_tests.cpp)
    target_link_libraries(output_tests PRIVATE cs2dumper_core)
    add_test(NAME output_tests COMMAND output_tests)

    add_executable(config_tests tests/config_tests.cpp)
    target_link_libraries(config_tests PRIVATE cs2dumper_core)
    add_test(NAME config_tests COMMAND config_tests)
endif()
