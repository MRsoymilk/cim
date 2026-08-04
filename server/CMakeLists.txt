cmake_minimum_required(VERSION 3.20)

find_package(SQLite3 REQUIRED)

set(USOCKETS_SOURCES
    ${CMAKE_SOURCE_DIR}/vendor/uWebSockets/uSockets/src/bsd.c
    ${CMAKE_SOURCE_DIR}/vendor/uWebSockets/uSockets/src/context.c
    ${CMAKE_SOURCE_DIR}/vendor/uWebSockets/uSockets/src/loop.c
    ${CMAKE_SOURCE_DIR}/vendor/uWebSockets/uSockets/src/quic.c
    ${CMAKE_SOURCE_DIR}/vendor/uWebSockets/uSockets/src/socket.c
    ${CMAKE_SOURCE_DIR}/vendor/uWebSockets/uSockets/src/udp.c
)

if(WIN32)
    list(APPEND USOCKETS_SOURCES ${CMAKE_SOURCE_DIR}/vendor/uWebSockets/uSockets/src/eventing/libuv.c)
else()
    list(APPEND USOCKETS_SOURCES ${CMAKE_SOURCE_DIR}/vendor/uWebSockets/uSockets/src/eventing/epoll_kqueue.c)
endif()

add_library(usockets STATIC ${USOCKETS_SOURCES})

target_compile_definitions(usockets PRIVATE LIBUS_NO_SSL)
target_include_directories(usockets PUBLIC 
    ${CMAKE_SOURCE_DIR}/vendor/uWebSockets/uSockets/src
)

add_executable(cim-server main.cpp Database.cpp)
target_include_directories(cim-server PRIVATE 
    ${CMAKE_SOURCE_DIR}/vendor/uWebSockets/src
    ${CMAKE_SOURCE_DIR}/vendor/uWebSockets/uSockets/src
    ${CMAKE_SOURCE_DIR}/vendor/nlohmann_json/single_include
)

if(WIN32)
    target_link_libraries(cim-server PRIVATE usockets SQLite3::SQLite3 sodium ws2_32 iphlpapi crypt32)
else()
    target_link_libraries(cim-server PRIVATE usockets SQLite3::SQLite3 sodium pthread z dl)
endif()

target_compile_features(cim-server PRIVATE cxx_std_20)
