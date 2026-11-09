cmake_minimum_required(VERSION 3.16)
project(my_netcpp_app LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Pulls netc++ straight from GitHub. Nothing else to install
# (on Windows libcurl is built automatically; on Linux install libcurl4-openssl-dev).
include(FetchContent)
FetchContent_Declare(netcpp
    GIT_REPOSITORY https://github.com/Unblonded/netcpp.git
    GIT_TAG v1.0.0
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(netcpp)

add_executable(app main.cpp)
target_link_libraries(app PRIVATE netcpp::netcpp)
