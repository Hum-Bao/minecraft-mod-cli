cmake_minimum_required(VERSION 3.20)
project(modrinth-cli CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)
include(ProcessorCount)

ProcessorCount(OPENSSL_BUILD_JOBS)
if(NOT OPENSSL_BUILD_JOBS)
    set(OPENSSL_BUILD_JOBS 2)
endif()

# OpenSSL
FetchContent_Declare(
    openssl
    URL https://github.com/openssl/openssl/releases/download/openssl-3.5.6/openssl-3.5.6.tar.gz
)

# Boost
FetchContent_Declare(
    boost
    URL https://github.com/boostorg/boost/releases/download/boost-1.90.0/boost-1.90.0-cmake.tar.gz
)

# nlohmann-json
FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
)

# cpp-httplib
FetchContent_Declare(
    cpp_httplib
    URL https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.44.0.tar.gz
)

FetchContent_GetProperties(openssl)
if(NOT openssl_POPULATED)
    FetchContent_Populate(openssl)
endif()

FetchContent_GetProperties(boost)
if(NOT boost_POPULATED)
    FetchContent_Populate(boost)
endif()

FetchContent_GetProperties(cpp_httplib)
if(NOT cpp_httplib_POPULATED)
    FetchContent_Populate(cpp_httplib)
endif()

FetchContent_MakeAvailable(nlohmann_json)

set(OPENSSL_BUILD_DIR "${CMAKE_BINARY_DIR}/_deps/openssl-build")
set(OPENSSL_INSTALL_DIR "${CMAKE_BINARY_DIR}/_deps/openssl-install")
set(OPENSSL_INCLUDE_DIR "${OPENSSL_INSTALL_DIR}/include")
set(OPENSSL_CRYPTO_LIBRARY "${OPENSSL_INSTALL_DIR}/lib/libcrypto.a")
set(OPENSSL_SSL_LIBRARY "${OPENSSL_INSTALL_DIR}/lib/libssl.a")

file(MAKE_DIRECTORY "${OPENSSL_INCLUDE_DIR}")
file(MAKE_DIRECTORY "${OPENSSL_INSTALL_DIR}/lib")

add_custom_command(
    OUTPUT
        "${OPENSSL_CRYPTO_LIBRARY}"
        "${OPENSSL_SSL_LIBRARY}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${OPENSSL_BUILD_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${OPENSSL_INSTALL_DIR}"
    COMMAND ${CMAKE_COMMAND} -E chdir "${OPENSSL_BUILD_DIR}"
            "${openssl_SOURCE_DIR}/config"
            --prefix=${OPENSSL_INSTALL_DIR}
            --openssldir=${OPENSSL_INSTALL_DIR}
            --libdir=lib
            no-tests
            no-shared
    COMMAND ${CMAKE_COMMAND} -E chdir "${OPENSSL_BUILD_DIR}"
            make -j${OPENSSL_BUILD_JOBS}
    COMMAND ${CMAKE_COMMAND} -E chdir "${OPENSSL_BUILD_DIR}"
            make install_sw
    COMMENT "Configuring and building OpenSSL 3.5.6"
    VERBATIM
)

add_custom_target(openssl_build
    DEPENDS
        "${OPENSSL_CRYPTO_LIBRARY}"
        "${OPENSSL_SSL_LIBRARY}"
)

add_library(OpenSSL::Crypto STATIC IMPORTED GLOBAL)
set_target_properties(OpenSSL::Crypto PROPERTIES
    IMPORTED_LOCATION "${OPENSSL_CRYPTO_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${OPENSSL_INCLUDE_DIR}"
)

if(UNIX AND NOT APPLE)
    set_property(TARGET OpenSSL::Crypto APPEND PROPERTY
        INTERFACE_LINK_LIBRARIES "dl;pthread"
    )
endif()

add_library(OpenSSL::SSL STATIC IMPORTED GLOBAL)
set_target_properties(OpenSSL::SSL PROPERTIES
    IMPORTED_LOCATION "${OPENSSL_SSL_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${OPENSSL_INCLUDE_DIR}"
    INTERFACE_LINK_LIBRARIES OpenSSL::Crypto
)

if(NOT TARGET Boost::headers)
    add_library(boost_headers INTERFACE)
    add_library(Boost::headers ALIAS boost_headers)
    target_include_directories(boost_headers INTERFACE "${boost_SOURCE_DIR}")
endif()

if(NOT TARGET cpp_httplib::cpp_httplib)
    add_library(cpp_httplib_headers INTERFACE)
    add_library(cpp_httplib::cpp_httplib ALIAS cpp_httplib_headers)
    target_include_directories(cpp_httplib_headers INTERFACE "${cpp_httplib_SOURCE_DIR}")
    target_compile_definitions(cpp_httplib_headers INTERFACE CPPHTTPLIB_OPENSSL_SUPPORT)
endif()

# Main executable
add_executable(modrinth-cli
    Main.cpp
    CliParser.cpp
    CommandHandlers.cpp
    FileScanner.cpp
    OutputFormatter.cpp
    UpdateEngine.cpp
    UpdateState.cpp
    ModrinthAPI.cpp
    CurseForgeAPI.cpp
    FileInfo.cpp
    ModDependency.cpp
    ModVersionInfo.cpp
)

add_dependencies(modrinth-cli openssl_build)

# Link libraries
target_link_libraries(modrinth-cli
    PRIVATE
        OpenSSL::Crypto
        OpenSSL::SSL
        Boost::headers
        nlohmann_json::nlohmann_json
    cpp_httplib::cpp_httplib
)

# Include directories
target_include_directories(modrinth-cli
    PRIVATE
    "${boost_SOURCE_DIR}"
)
