add_library(led_common
    src/control_messages.cpp
    src/h264_annex_b.cpp
    src/h264_rtp_depacketizer.cpp
    src/h264_rtp_packetizer.cpp
    src/logger.cpp
    src/mjpeg_packet.cpp
    src/messages.cpp
    src/rtp_codec.cpp
    src/session_state.cpp
    src/tcp_socket.cpp
    src/udp_socket.cpp)

target_include_directories(led_common
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_compile_features(led_common PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(led_common PRIVATE /W4)
else()
    target_compile_options(led_common PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
    target_link_libraries(led_common PUBLIC ws2_32)
endif()
