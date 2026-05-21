add_library(led_client_core
    client-core/src/decoder.cpp
    client-core/src/discovery.cpp
    client-core/src/input_capture.cpp
    client-core/src/x11_input_capture.cpp
    client-core/src/input_sender.cpp
    client-core/src/renderer.cpp
    client-core/src/signaling_client.cpp
    client-core/src/session_client.cpp
    client-core/src/video_receiver.cpp)

target_include_directories(led_client_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/client-core/include)

target_link_libraries(led_client_core PUBLIC led_common)
target_compile_features(led_client_core PUBLIC cxx_std_20)

find_package(Threads REQUIRED)
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(GSTREAMER QUIET gstreamer-1.0)
endif()

if(GSTREAMER_FOUND)
    target_compile_definitions(led_client_core PUBLIC LED_HAS_GSTREAMER=1)
    target_include_directories(led_client_core PRIVATE ${GSTREAMER_INCLUDE_DIRS})
    target_link_directories(led_client_core PRIVATE ${GSTREAMER_LIBRARY_DIRS})
    target_link_libraries(led_client_core PRIVATE ${GSTREAMER_LIBRARIES})
endif()

if(PkgConfig_FOUND)
    pkg_check_modules(X11 QUIET x11)
    pkg_check_modules(XEXT QUIET xext)
    pkg_check_modules(GL QUIET gl)
endif()

if(X11_FOUND)
    target_compile_definitions(led_client_core PUBLIC LED_HAS_X11=1)
    target_include_directories(led_client_core PRIVATE ${X11_INCLUDE_DIRS})
    target_link_directories(led_client_core PRIVATE ${X11_LIBRARY_DIRS})
    target_link_libraries(led_client_core PRIVATE ${X11_LIBRARIES})
    if(XEXT_FOUND)
        target_compile_definitions(led_client_core PUBLIC LED_HAS_XSHM=1)
        target_include_directories(led_client_core PRIVATE ${XEXT_INCLUDE_DIRS})
        target_link_directories(led_client_core PRIVATE ${XEXT_LIBRARY_DIRS})
        target_link_libraries(led_client_core PRIVATE ${XEXT_LIBRARIES})
    endif()
    if(GL_FOUND)
        target_compile_definitions(led_client_core PUBLIC LED_HAS_GLX=1)
        target_include_directories(led_client_core PRIVATE ${GL_INCLUDE_DIRS})
        target_link_directories(led_client_core PRIVATE ${GL_LIBRARY_DIRS})
        target_link_libraries(led_client_core PRIVATE ${GL_LIBRARIES})
    endif()
endif()

if(MSVC)
    target_compile_options(led_client_core PRIVATE /W4)
else()
    target_compile_options(led_client_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(led_client_app
    client-ui/client_app.cpp)

target_link_libraries(led_client_app PRIVATE led_client_core Threads::Threads)

find_package(JPEG QUIET)
if(JPEG_FOUND)
    target_compile_definitions(led_client_app PRIVATE LED_HAS_LIBJPEG=1)
    target_include_directories(led_client_app PRIVATE ${JPEG_INCLUDE_DIRS})
    target_link_libraries(led_client_app PRIVATE ${JPEG_LIBRARIES})
endif()
