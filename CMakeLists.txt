cmake_minimum_required(VERSION 3.21)
project(cim VERSION 0.1.0 LANGUAGES C CXX)

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

find_package(unofficial-sodium CONFIG QUIET)
if(TARGET unofficial-sodium::sodium)
    set(CIM_SODIUM_TARGET unofficial-sodium::sodium)
else()
    find_path(CIM_SODIUM_INCLUDE_DIR NAMES sodium.h REQUIRED)
    find_library(CIM_SODIUM_LIBRARY NAMES sodium libsodium REQUIRED)
    add_library(cim-sodium UNKNOWN IMPORTED)
    set_target_properties(cim-sodium PROPERTIES
        IMPORTED_LOCATION "${CIM_SODIUM_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${CIM_SODIUM_INCLUDE_DIR}"
    )
    set(CIM_SODIUM_TARGET cim-sodium)
endif()

add_executable(cim 
    src/main.cpp
    src/ClientConfig.cpp
    src/ui/ChatUI.cpp
)

target_include_directories(cim PRIVATE src vendor/nlohmann_json/single_include)
target_link_libraries(cim PRIVATE
    ftxui::component
    ixwebsocket::ixwebsocket
    ${CIM_SODIUM_TARGET}
)

add_subdirectory(server)
