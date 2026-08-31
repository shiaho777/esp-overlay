cmake_minimum_required(VERSION 3.10.2)
project("esp-overlay")

# C++17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Native bridge library
add_library(esp-native SHARED
    native_bridge.cpp
    memory_reader.cpp
    il2cpp_resolver.cpp
    offset_resolver.cpp
    offset_table.cpp
    esp_engine.cpp
)

# Find log and android libraries
find_library(log-lib log)
find_library(android-lib android)

# Link
target_link_libraries(esp-native
    ${log-lib}
    ${android-lib}
)

# Optimize for speed (ARM64)
target_compile_options(esp-native PRIVATE
    -O2
    -ffast-math
    -fvisibility=hidden
    -Wall
    -Wno-unused-parameter
)
