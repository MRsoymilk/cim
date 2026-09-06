cmake_minimum_required(VERSION 3.20)

find_package(SQLite3 REQUIRED)

add_executable(cim-server main.cpp Database.cpp)
target_include_directories(cim-server PRIVATE
    ${CMAKE_SOURCE_DIR}/vendor/nlohmann_json/single_include
)
target_link_libraries(cim-server PRIVATE
    ixwebsocket::ixwebsocket
    SQLite3::SQLite3
    sodium
)
target_compile_features(cim-server PRIVATE cxx_std_20)
