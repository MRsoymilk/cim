cmake_minimum_required(VERSION 3.20)

add_library(usockets STATIC
    ${CMAKE_SOURCE_DIR}/vendor/uWebSockets/uSockets/src/bsd.c
    ${CMAKE_SOURCE_DIR}/vendor/uWebSockets/uSockets/src/context.c
    ${CMAKE_SOURCE_DIR}/vendor/uWebSockets/uSockets/src/loop.c
    ${CMAKE_SOURCE_DIR}/vendor/uWebSockets/uSockets/src/quic.c
    ${CMAKE_SOURCE_DIR}/vendor/uWebSockets/uSockets/src/socket.c
    ${CMAKE_SOURCE_DIR}/vendor/uWebSockets/uSockets/src/udp.c
    ${CMAKE_SOURCE_DIR}/vendor/uWebSockets/uSockets/src/eventing/epoll_kqueue.c
)

target_compile_definitions(usockets PRIVATE LIBUS_NO_SSL)
target_include_directories(usockets PUBLIC 
    ${CMAKE_SOURCE_DIR}/vendor/uWebSockets/uSockets/src
)

add_executable(cim-server main.cpp)
target_include_directories(cim-server PRIVATE 
    ${CMAKE_SOURCE_DIR}/vendor/uWebSockets/src
    ${CMAKE_SOURCE_DIR}/vendor/uWebSockets/uSockets/src
    ${CMAKE_SOURCE_DIR}/vendor/nlohmann_json/single_include
)
target_link_libraries(cim-server PRIVATE usockets pthread z)
target_compile_features(cim-server PRIVATE cxx_std_20)
