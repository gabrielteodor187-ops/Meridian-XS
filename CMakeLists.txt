cmake_minimum_required(VERSION 3.24)
project(MeridianXS VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(meridian-xs
    src/main.cpp
)

target_include_directories(meridian-xs PRIVATE include)

if(MSVC)
    target_compile_options(meridian-xs PRIVATE /W4 /permissive-)
else()
    target_compile_options(meridian-xs PRIVATE -Wall -Wextra -Wpedantic)
endif()
