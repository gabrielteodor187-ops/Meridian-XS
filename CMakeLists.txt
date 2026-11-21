cmake_minimum_required(VERSION 3.24)
project(MeridianXS VERSION 0.44.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(MERIDIAN_CORE_SOURCES
    src/core/guest_memory.cpp
    src/core/virtual_address_space.cpp
    src/core/x64_page_table_walker.cpp
    src/core/x64_paged_memory.cpp
    src/cpu/cpu_state.cpp
    src/cpu/interpreter.cpp
)

add_executable(meridian-xs src/main.cpp ${MERIDIAN_CORE_SOURCES})
target_include_directories(meridian-xs PRIVATE include)

if(MSVC)
    target_compile_options(meridian-xs PRIVATE /W4 /permissive-)
else()
    target_compile_options(meridian-xs PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(meridian-core-tests tests/core_tests.cpp ${MERIDIAN_CORE_SOURCES})
    target_include_directories(meridian-core-tests PRIVATE include)
    if(MSVC)
        target_compile_options(meridian-core-tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(meridian-core-tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME meridian-core-tests COMMAND meridian-core-tests)
endif()
