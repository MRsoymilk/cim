cmake_minimum_required(VERSION 3.21)
project(cim VERSION 0.0.6 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

set(FTXUI_QUIET ON CACHE BOOL "" FORCE)
add_subdirectory(vendor/FTXUI)

set(BUILD_DEMO OFF CACHE BOOL "" FORCE)
set(IXWEBSOCKET_INSTALL OFF CACHE BOOL "" FORCE)
set(USE_TEST OFF CACHE BOOL "" FORCE)
set(USE_TLS OFF CACHE BOOL "" FORCE)
set(USE_WS OFF CACHE BOOL "" FORCE)
set(USE_ZLIB OFF CACHE BOOL "" FORCE)
add_subdirectory(vendor/IXWebSocket)

add_executable(cim 
    src/main.cpp
    src/ui/ChatUI.cpp
)

target_include_directories(cim PRIVATE src vendor/nlohmann_json/single_include)
target_link_libraries(cim PRIVATE ftxui::component ixwebsocket::ixwebsocket)
if(WIN32)
    target_link_libraries(cim PRIVATE ws2_32)
endif()

add_subdirectory(server)
