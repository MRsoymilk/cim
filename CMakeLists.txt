cmake_minimum_required(VERSION 3.20)
project(cim VERSION 0.0.2 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

add_subdirectory(vendor/FTXUI)

add_executable(cim 
    src/main.cpp
    src/ui/ChatUI.cpp
)

target_include_directories(cim PRIVATE src)
target_link_libraries(cim PRIVATE ftxui::component)

add_subdirectory(server)
