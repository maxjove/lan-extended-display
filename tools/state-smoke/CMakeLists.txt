add_executable(led_state_smoke
    state_smoke.cpp)

target_link_libraries(led_state_smoke PRIVATE led_common)

add_executable(led_annex_b_smoke
    annex_b_smoke.cpp)

target_link_libraries(led_annex_b_smoke PRIVATE led_common)

add_executable(led_rtp_packetizer_smoke
    rtp_packetizer_smoke.cpp)

target_link_libraries(led_rtp_packetizer_smoke PRIVATE led_common)

add_executable(led_udp_rtp_loopback_smoke
    udp_rtp_loopback_smoke.cpp)

target_link_libraries(led_udp_rtp_loopback_smoke PRIVATE led_common)

add_executable(led_tcp_signaling_loopback_smoke
    tcp_signaling_loopback_smoke.cpp)

find_package(Threads REQUIRED)

target_link_libraries(led_tcp_signaling_loopback_smoke PRIVATE led_common Threads::Threads)
