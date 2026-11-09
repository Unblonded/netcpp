cmake_minimum_required(VERSION 3.16)

project(netcpp
    VERSION 1.0.0
    DESCRIPTION "netc++ - cross-platform C++ network utility library"
    LANGUAGES CXX)

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(NETCPP_TOP_LEVEL ON)
else()
    set(NETCPP_TOP_LEVEL OFF)
endif()

option(NETCPP_BUILD_EXAMPLES "Build netc++ examples" ${NETCPP_TOP_LEVEL})
option(NETCPP_BUILD_TESTS "Build netc++ tests" ${NETCPP_TOP_LEVEL})
option(NETCPP_INSTALL "Generate install rules" ${NETCPP_TOP_LEVEL})

include(GNUInstallDirs)

find_package(Threads REQUIRED)
find_package(CURL REQUIRED)

add_library(netcpp
    src/platform.cpp
    src/encoding.cpp
    src/url.cpp
    src/sockets.cpp
    src/tls.cpp
    src/dns.cpp
    src/http.cpp
    src/websocket.cpp
    src/utils.cpp)
add_library(netcpp::netcpp ALIAS netcpp)

target_compile_features(netcpp PUBLIC cxx_std_17)
target_include_directories(netcpp
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(netcpp PUBLIC Threads::Threads PRIVATE CURL::libcurl)
set_target_properties(netcpp PROPERTIES
    OUTPUT_NAME netcpp
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    WINDOWS_EXPORT_ALL_SYMBOLS ON)

if(WIN32)
    target_link_libraries(netcpp PRIVATE ws2_32 iphlpapi)
    target_compile_definitions(netcpp PRIVATE _WIN32_WINNT=0x0601 _CRT_SECURE_NO_WARNINGS)
endif()

if(MSVC)
    target_compile_options(netcpp PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(netcpp PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(NETCPP_BUILD_EXAMPLES)
    foreach(example http_client websocket_chat tcp_echo_server port_scanner netinfo)
        add_executable(${example} examples/${example}.cpp)
        target_link_libraries(${example} PRIVATE netcpp::netcpp)
    endforeach()
endif()

if(NETCPP_BUILD_TESTS)
    enable_testing()
    add_executable(netcpp_tests tests/test_main.cpp)
    target_link_libraries(netcpp_tests PRIVATE netcpp::netcpp)
    add_test(NAME netcpp_tests COMMAND netcpp_tests)
endif()

if(NETCPP_INSTALL)
    include(CMakePackageConfigHelpers)

    install(TARGETS netcpp EXPORT netcppTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(DIRECTORY include/netcpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(EXPORT netcppTargets
        NAMESPACE netcpp::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/netcpp)

    configure_package_config_file(cmake/netcppConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/netcppConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/netcpp)
    write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/netcppConfigVersion.cmake
        COMPATIBILITY SameMajorVersion)
    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/netcppConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/netcppConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/netcpp)
endif()
