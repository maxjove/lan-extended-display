cmake_minimum_required(VERSION 3.22)

project(lan_extended_display
    VERSION 0.1.0
    DESCRIPTION "Low-latency LAN extended display"
    LANGUAGES CXX)

option(LED_BUILD_HOST "Build Windows host components" ON)
option(LED_BUILD_CLIENT "Build Linux ARM64 client components" ON)
option(LED_BUILD_TOOLS "Build developer tools" ON)
option(LED_BUILD_IDD_DRIVER "Build the Windows IddCx virtual display driver" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_subdirectory(common)

if(LED_BUILD_HOST)
    add_subdirectory(windows-host)
endif()

if(LED_BUILD_CLIENT)
    add_subdirectory(linux-client)
endif()

if(LED_BUILD_TOOLS)
    add_subdirectory(tools/state-smoke)
endif()
