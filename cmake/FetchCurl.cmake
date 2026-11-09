# Downloads and builds a static libcurl as part of this build so netc++ works
# without any system packages (used automatically on Windows when libcurl is
# not installed, or always with -DNETCPP_FETCH_CURL=ON).
#
# TLS backend: Schannel on Windows, SecureTransport on macOS, OpenSSL on Linux.

include(FetchContent)

message(STATUS "netc++: building libcurl 8.11.1 from source")

# Let our plain variables override curl's option() defaults.
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
set(CMAKE_POLICY_DEFAULT_CMP0126 NEW)

set(BUILD_SHARED_LIBS OFF)
set(BUILD_STATIC_LIBS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(BUILD_CURL_EXE OFF)
set(BUILD_TESTING OFF)
set(BUILD_LIBCURL_DOCS OFF)
set(BUILD_MISC_DOCS OFF)
set(ENABLE_CURL_MANUAL OFF)
set(CURL_DISABLE_INSTALL ON)
set(CURL_USE_LIBPSL OFF)
set(CURL_USE_LIBSSH2 OFF)
set(CURL_USE_LIBSSH OFF)
set(CURL_USE_GSSAPI OFF)
set(CURL_ZLIB OFF)
set(CURL_BROTLI OFF)
set(CURL_ZSTD OFF)
set(USE_NGHTTP2 OFF)
set(USE_LIBIDN2 OFF)
set(USE_WIN32_IDN OFF)
set(CURL_DISABLE_LDAP ON)
set(CURL_DISABLE_LDAPS ON)
set(ENABLE_UNICODE OFF)

if(WIN32)
    set(CURL_USE_SCHANNEL ON)
    set(CURL_USE_OPENSSL OFF)
elseif(APPLE)
    set(CURL_USE_SECTRANSP ON)
    set(CURL_USE_OPENSSL OFF)
    set(CURL_CA_FALLBACK ON)
else()
    set(CURL_USE_OPENSSL ON)
endif()

FetchContent_Declare(netcpp_curl
    URL https://github.com/curl/curl/releases/download/curl-8_11_1/curl-8.11.1.tar.xz
    URL_HASH SHA256=c7ca7db48b0909743eaef34250da02c19bc61d4f1dcedd6603f109409536ab56
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(netcpp_curl)

# Restore the caller's choice (cache value) for everything that follows.
unset(BUILD_SHARED_LIBS)
unset(BUILD_STATIC_LIBS)
unset(BUILD_TESTING)

if(NOT TARGET CURL::libcurl)
    if(TARGET libcurl_static)
        add_library(CURL::libcurl ALIAS libcurl_static)
    elseif(TARGET libcurl)
        add_library(CURL::libcurl ALIAS libcurl)
    else()
        message(FATAL_ERROR "netc++: bundled libcurl did not produce a usable target")
    endif()
endif()
