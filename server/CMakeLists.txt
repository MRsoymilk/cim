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

add_executable(cim-server main.cpp Database.cpp)
target_include_directories(cim-server PRIVATE
    ${CMAKE_SOURCE_DIR}/vendor/nlohmann_json/single_include
)
target_link_libraries(cim-server PRIVATE
    ixwebsocket::ixwebsocket
    ${CIM_SQLITE_TARGET}
    ${CIM_SODIUM_TARGET}
)
target_compile_features(cim-server PRIVATE cxx_std_20)
