add_library(led_host_core
    host-core/src/capture_engine.cpp
    host-core/src/cursor_suppressor.cpp
    host-core/src/display_manager.cpp
    host-core/src/encoder.cpp
    host-core/src/input_injector.cpp
    host-core/src/input_receiver.cpp
    host-core/src/jpeg_encoder.cpp
    host-core/src/signaling_server.cpp
    host-core/src/session_manager.cpp
    host-core/src/video_sender.cpp)

target_include_directories(led_host_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/host-core/include)

target_link_libraries(led_host_core PUBLIC led_common)
target_compile_features(led_host_core PUBLIC cxx_std_20)

find_package(Threads REQUIRED)

set(LED_TURBOJPEG_INCLUDE_DIR "" CACHE PATH "Optional TurboJPEG include directory")
set(LED_TURBOJPEG_LIBRARY "" CACHE FILEPATH "Optional TurboJPEG library")
if(NOT LED_TURBOJPEG_INCLUDE_DIR)
    find_path(LED_TURBOJPEG_INCLUDE_DIR turbojpeg.h)
endif()
if(NOT LED_TURBOJPEG_LIBRARY)
    find_library(LED_TURBOJPEG_LIBRARY NAMES turbojpeg turbojpeg-static)
endif()
if(LED_TURBOJPEG_INCLUDE_DIR AND LED_TURBOJPEG_LIBRARY)
    target_compile_definitions(led_host_core PRIVATE LED_HAS_TURBOJPEG=1)
    target_include_directories(led_host_core PRIVATE ${LED_TURBOJPEG_INCLUDE_DIR})
    target_link_libraries(led_host_core PRIVATE ${LED_TURBOJPEG_LIBRARY})
    message(STATUS "Windows host TurboJPEG enabled: ${LED_TURBOJPEG_LIBRARY}")
else()
    message(STATUS "Windows host TurboJPEG not found; using WIC/GDI+ JPEG fallback")
endif()

if(MSVC)
    target_compile_options(led_host_core PRIVATE /W4)
else()
    target_compile_options(led_host_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(led_host_app
    host-ui/host_app.cpp)

target_link_libraries(led_host_app PRIVATE led_host_core Threads::Threads)

if(WIN32)
    add_executable(led_host_tray WIN32
        host-ui/tray_app.cpp)

    target_compile_features(led_host_tray PRIVATE cxx_std_20)
    target_link_libraries(led_host_tray PRIVATE shell32 user32 ws2_32 iphlpapi)
endif()

if(MSVC)
    target_link_libraries(led_host_core PRIVATE
        mfplat
        mf
        mfuuid
        wmcodecdspuuid
        gdiplus
        windowscodecs
        d3d11
        dxgi
        ole32)
endif()

if(LED_BUILD_IDD_DRIVER)
    add_subdirectory(driver/idd-virtual-display)
endif()
