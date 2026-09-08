cmake_minimum_required(VERSION 3.21)
project(cim VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

find_package(OpenSSL REQUIRED)

option(CIM_BUILD_CLIENT "Build the cim terminal client" ON)

if(CIM_BUILD_CLIENT)
    set(FTXUI_QUIET ON CACHE BOOL "" FORCE)
    add_subdirectory(vendor/FTXUI)
endif()

set(BUILD_DEMO OFF CACHE BOOL "" FORCE)
set(IXWEBSOCKET_INSTALL OFF CACHE BOOL "" FORCE)
set(USE_TEST OFF CACHE BOOL "" FORCE)
set(USE_TLS ON CACHE BOOL "" FORCE)
set(USE_OPEN_SSL ON CACHE BOOL "" FORCE)
set(USE_WS OFF CACHE BOOL "" FORCE)
set(USE_ZLIB OFF CACHE BOOL "" FORCE)
add_subdirectory(vendor/IXWebSocket)

if(CIM_BUILD_CLIENT)
    set(CIM_CA_CERT "" CACHE FILEPATH "Private CA certificate distributed with the cim client")
    if(NOT CIM_CA_CERT)
        message(FATAL_ERROR "CIM_CA_CERT is required; pass -DCIM_CA_CERT=/path/to/ca.crt")
    endif()
    if(NOT EXISTS "${CIM_CA_CERT}" OR IS_DIRECTORY "${CIM_CA_CERT}")
        message(FATAL_ERROR "CIM_CA_CERT is not a certificate file: ${CIM_CA_CERT}")
    endif()
    get_filename_component(CIM_CA_CERT "${CIM_CA_CERT}" ABSOLUTE)
    file(SIZE "${CIM_CA_CERT}" CIM_CA_CERT_SIZE)
    if(CIM_CA_CERT_SIZE GREATER 4194304)
        message(FATAL_ERROR "CIM_CA_CERT exceeds the 4 MiB safety limit")
    endif()
    file(READ "${CIM_CA_CERT}" CIM_CA_CERT_CONTENT)
    if(CIM_CA_CERT_CONTENT MATCHES "PRIVATE KEY" OR
       NOT CIM_CA_CERT_CONTENT MATCHES "-----BEGIN CERTIFICATE-----")
        message(FATAL_ERROR "CIM_CA_CERT must contain CA certificates only, never private keys")
    endif()
    string(REGEX MATCHALL "-----BEGIN CERTIFICATE-----" CIM_CA_CERT_BEGIN_MARKERS
           "${CIM_CA_CERT_CONTENT}")
    string(REGEX MATCHALL "-----END CERTIFICATE-----" CIM_CA_CERT_END_MARKERS
           "${CIM_CA_CERT_CONTENT}")
    list(LENGTH CIM_CA_CERT_BEGIN_MARKERS CIM_CA_CERT_BEGIN_COUNT)
    list(LENGTH CIM_CA_CERT_END_MARKERS CIM_CA_CERT_END_COUNT)
    if(NOT CIM_CA_CERT_BEGIN_COUNT EQUAL 1 OR NOT CIM_CA_CERT_END_COUNT EQUAL 1)
        message(FATAL_ERROR "CIM_CA_CERT must contain exactly one CA certificate")
    endif()
    find_program(CIM_OPENSSL_EXECUTABLE NAMES openssl REQUIRED)
    execute_process(
        COMMAND "${CIM_OPENSSL_EXECUTABLE}" x509 -in "${CIM_CA_CERT}" -noout
        RESULT_VARIABLE CIM_CA_PARSE_RESULT
        ERROR_QUIET
    )
    execute_process(
        COMMAND "${CIM_OPENSSL_EXECUTABLE}" x509 -in "${CIM_CA_CERT}" -noout -ext basicConstraints
        RESULT_VARIABLE CIM_CA_CONSTRAINTS_RESULT
        OUTPUT_VARIABLE CIM_CA_CONSTRAINTS
        ERROR_QUIET
    )
    if(NOT CIM_CA_PARSE_RESULT EQUAL 0 OR NOT CIM_CA_CONSTRAINTS_RESULT EQUAL 0 OR
       NOT CIM_CA_CONSTRAINTS MATCHES "CA:TRUE")
        message(FATAL_ERROR "CIM_CA_CERT is not a valid CA certificate: ${CIM_CA_CERT}")
    endif()
    unset(CIM_CA_CERT_CONTENT)
    unset(CIM_CA_CONSTRAINTS)
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

if(CIM_BUILD_CLIENT)
    add_executable(cim
        src/main.cpp
        src/config/ClientConfig.cpp
        src/net/ClientConnection.cpp
        src/ui/chat/ChatUI.cpp
        src/ui/chat/ChatUIAuth.cpp
        src/ui/chat/ChatUISettings.cpp
        src/ui/chat/ChatUIProtocol.cpp
        src/ui/chat/ChatUIContacts.cpp
        src/ui/chat/ChatUIRender.cpp
    )

    target_include_directories(cim PRIVATE src vendor/nlohmann_json/single_include)
    target_link_libraries(cim PRIVATE
        ftxui::component
        ixwebsocket::ixwebsocket
        ${CIM_SODIUM_TARGET}
    )
    add_custom_target(cim-ca ALL
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CIM_CA_CERT}"
            "$<TARGET_FILE_DIR:cim>/cim-ca.crt"
        COMMENT "Copying the CIM client trust certificate"
    )
    add_dependencies(cim cim-ca)
endif()

add_subdirectory(server)
