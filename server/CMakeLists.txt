cmake_minimum_required(VERSION 3.20)

find_package(unofficial-sqlite3 CONFIG QUIET)
if(TARGET unofficial::sqlite3::sqlite3)
    set(CIM_SQLITE_TARGET unofficial::sqlite3::sqlite3)
else()
    find_package(SQLite3 REQUIRED)
    if(TARGET SQLite3::SQLite3)
        set(CIM_SQLITE_TARGET SQLite3::SQLite3)
    elseif(TARGET SQLite::SQLite3)
        set(CIM_SQLITE_TARGET SQLite::SQLite3)
    else()
        message(FATAL_ERROR "SQLite3 was found but did not provide a CMake target")
    endif()
endif()

add_executable(cim-server main.cpp Database.cpp AdminCLI.cpp)
target_include_directories(cim-server PRIVATE
    ${CMAKE_SOURCE_DIR}/vendor/nlohmann_json/single_include
)
target_link_libraries(cim-server PRIVATE
    ixwebsocket::ixwebsocket
    ${CIM_SQLITE_TARGET}
    ${CIM_SODIUM_TARGET}
)
target_compile_features(cim-server PRIVATE cxx_std_20)
