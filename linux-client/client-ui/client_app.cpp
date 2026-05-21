#include "led/client/discovery.h"
#include "led/client/decoder.h"
#include "led/client/input_capture.h"
#include "led/client/input_sender.h"
#include "led/client/signaling_client.h"
#include "led/client/session_client.h"
#include "led/client/video_receiver.h"
#include "led/client/x11_input_capture.h"
#include "led/common/logger.h"
#include "led/protocol/control_messages.h"
#include "led/protocol/messages.h"
#include "led/session/session_state.h"
#include "led/transport/h264_annex_b.h"
#include "led/transport/mjpeg_packet.h"
#include "led/transport/tcp_socket.h"
#include "led/transport/udp_socket.h"

#if defined(LED_HAS_LIBJPEG)
#include <cstddef>
#include <cstdio>
extern "C" {
#include <jpeglib.h>
}
#include <setjmp.h>
#endif

#include <chrono>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr std::uint32_t kStartupReceiveTimeoutMs = 5000;
constexpr std::uint32_t kActiveReceiveTimeoutMs = 1000;
constexpr std::uint32_t kStartupIdleTimeouts = 12;
constexpr std::uint16_t kFrameAckPort = 17692;

std::uint16_t parsePort(const char* value, std::uint16_t fallback) {
    try {
        const auto parsed = static_cast<unsigned long>(std::stoul(value));
        if (parsed <= 65535) {
            return static_cast<std::uint16_t>(parsed);
        }
    } catch (...) {
    }
    return fallback;
}

std::uint32_t parseU32(const char* value, std::uint32_t fallback) {
    try {
        return static_cast<std::uint32_t>(std::stoul(value));
    } catch (...) {
    }
    return fallback;
}

void sendFrameAck(
    led::transport::UdpSocket& socket,
    const led::transport::UdpEndpoint& endpoint,
    const char* stage,
    std::uint64_t frameId,
    std::uint64_t& lastAckedFrameId) {
    if (frameId == 0 || frameId == lastAckedFrameId || !socket.isOpen()) {
        return;
    }
    const auto message = std::string("LED_FRAME_ACK_V1;stage=") + stage + ";frame=" + std::to_string(frameId) + ";";
    const std::vector<std::uint8_t> bytes(message.begin(), message.end());
    const auto status = socket.sendTo(bytes, endpoint);
    if (status.isOk()) {
        lastAckedFrameId = frameId;
    }
}

#if defined(LED_HAS_LIBJPEG)
struct JpegDecodeError {
    jpeg_error_mgr manager;
    jmp_buf jumpBuffer;
    char message[JMSG_LENGTH_MAX]{};
};

void onJpegDecodeError(j_common_ptr jpeg) {
    auto* error = reinterpret_cast<JpegDecodeError*>(jpeg->err);
    (*jpeg->err->format_message)(jpeg, error->message);
    longjmp(error->jumpBuffer, 1);
}

led::Status decodeJpegToBgrx(
    const std::vector<std::uint8_t>& jpegBytes,
    std::uint64_t frameId,
    led::client::RawFrameInfo& frame,
    std::vector<std::uint8_t>& pixels) {
    if (jpegBytes.empty()) {
        return led::Status::invalidArgument("cannot decode empty JPEG frame");
    }

    jpeg_decompress_struct decoder{};
    JpegDecodeError error{};
    decoder.err = jpeg_std_error(&error.manager);
    error.manager.error_exit = onJpegDecodeError;
    if (setjmp(error.jumpBuffer) != 0) {
        jpeg_destroy_decompress(&decoder);
        return led::Status::internalError(std::string("JPEG decode failed: ") + error.message);
    }

    jpeg_create_decompress(&decoder);
    jpeg_mem_src(
        &decoder,
        const_cast<unsigned char*>(reinterpret_cast<const unsigned char*>(jpegBytes.data())),
        static_cast<unsigned long>(jpegBytes.size()));
    jpeg_read_header(&decoder, TRUE);
    decoder.out_color_space = JCS_EXT_BGRX;
    jpeg_start_decompress(&decoder);

    const auto width = static_cast<std::uint32_t>(decoder.output_width);
    const auto height = static_cast<std::uint32_t>(decoder.output_height);
    const auto stride = width * 4;
    pixels.assign(static_cast<std::size_t>(stride) * height, 0);
    while (decoder.output_scanline < decoder.output_height) {
        auto* row = pixels.data() + static_cast<std::size_t>(decoder.output_scanline) * stride;
        JSAMPROW rows[] = {row};
        jpeg_read_scanlines(&decoder, rows, 1);
    }
    jpeg_finish_decompress(&decoder);
    jpeg_destroy_decompress(&decoder);

    frame.width = width;
    frame.height = height;
    frame.stride = stride;
    frame.bytes = pixels.size();
    frame.ptsNs = frameId * 1000000ULL;
    frame.format = "BGRx";
    return led::Status::ok();
}
#endif

std::string sinkPipelineFromMode(const std::string& mode) {
    if (mode == "auto") {
        return "decodebin ! videoconvert ! autovideosink sync=false";
    }
    if (mode == "avdec") {
        return "avdec_h264 ! videoconvert ! autovideosink sync=false";
    }
    if (mode == "avdec-display-probe") {
        return "avdec_h264 ! videoconvert ! tee name=t "
               "t. ! queue leaky=downstream max-size-buffers=1 max-size-bytes=0 max-size-time=0 "
               "! fakesink name=decoded_sink signal-handoffs=true sync=false "
               "t. ! queue leaky=downstream max-size-buffers=1 max-size-bytes=0 max-size-time=0 "
               "! autovideosink sync=false";
    }
    if (mode == "avdec-x11") {
        return "avdec_h264 ! videoconvert ! ximagesink sync=false";
    }
    if (mode == "avdec-xv") {
        return "avdec_h264 ! videoconvert ! xvimagesink sync=false";
    }
    if (mode == "avdec-gl") {
        return "avdec_h264 ! videoconvert ! glimagesink sync=false";
    }
    if (mode == "avdec-wayland") {
        return "avdec_h264 ! videoconvert ! waylandsink sync=false";
    }
    if (mode == "avdec-vulkan") {
        return "avdec_h264 ! videoconvert ! vulkansink sync=false";
    }
    if (mode == "avdec-kms") {
        return "avdec_h264 ! videoconvert ! kmssink sync=false";
    }
    if (mode == "avdec-fb") {
        return "avdec_h264 ! videoconvert ! fbdevsink sync=false";
    }
    if (mode == "avdec-fps") {
        return "avdec_h264 ! videoconvert ! fpsdisplaysink text-overlay=false video-sink=autovideosink sync=false";
    }
    if (mode == "avdec-fake") {
        return "avdec_h264 ! fakesink sync=false";
    }
    if (mode == "avdec-probe") {
        return "avdec_h264 ! videoconvert ! fakesink name=decoded_sink signal-handoffs=true sync=false";
    }
    if (mode == "avdec-x11raw") {
        return "avdec_h264 ! videoconvert ! video/x-raw,format=BGRx "
               "! fakesink name=decoded_sink signal-handoffs=true sync=false";
    }
    if (mode == "avdec-i420gl") {
        return "avdec_h264 ! video/x-raw,format=I420 "
               "! fakesink name=decoded_sink signal-handoffs=true sync=false";
    }
    if (mode == "vaapi-nv12gl") {
        return "video/x-h264,stream-format=byte-stream,alignment=au "
               "! vaapih264dec ! vaapipostproc ! video/x-raw,format=NV12 "
               "! fakesink name=decoded_sink signal-handoffs=true sync=false";
    }
    if (mode == "vaapi-lowlatency-nv12gl") {
        return "video/x-h264,stream-format=byte-stream,alignment=au "
               "! vaapih264dec low-latency=true ! vaapipostproc ! video/x-raw,format=NV12 "
               "! fakesink name=decoded_sink signal-handoffs=true sync=false";
    }
    if (mode == "vaapi-bin-nv12gl") {
        return "video/x-h264,stream-format=byte-stream,alignment=au "
               "! vaapidecodebin max-size-buffers=1 max-size-bytes=0 max-size-time=0 disable-vpp=true "
               "! vaapipostproc ! video/x-raw,format=NV12 "
               "! fakesink name=decoded_sink signal-handoffs=true sync=false";
    }
    if (mode == "vaapi-avc-nv12gl") {
        return "video/x-h264,stream-format=avc,alignment=au "
               "! vaapih264dec ! vaapipostproc ! video/x-raw,format=NV12 "
               "! fakesink name=decoded_sink signal-handoffs=true sync=false";
    }
    if (mode == "vaapi-sink") {
        return "video/x-h264,stream-format=byte-stream,alignment=au "
               "! vaapih264dec ! vaapisink name=decoded_sink signal-handoffs=true sync=false fullscreen=true";
    }
    if (mode == "vaapi-lowlatency-sink") {
        return "video/x-h264,stream-format=byte-stream,alignment=au "
               "! vaapih264dec low-latency=true ! vaapisink name=decoded_sink signal-handoffs=true sync=false fullscreen=true";
    }
    if (mode == "omx-i420gl") {
        return "avdec_h264_omx_dec ! videoconvert ! video/x-raw,format=I420 "
               "! fakesink name=decoded_sink signal-handoffs=true sync=false";
    }
    if (mode == "omx-avc-i420gl") {
        return "video/x-h264,stream-format=avc,alignment=au "
               "! avdec_h264_omx_dec ! videoconvert ! video/x-raw,format=I420 "
               "! fakesink name=decoded_sink signal-handoffs=true sync=false";
    }
    if (mode == "ftomx-i420gl") {
        return "avdec_h264ftomx ! videoconvert ! video/x-raw,format=I420 "
               "! fakesink name=decoded_sink signal-handoffs=true sync=false";
    }
    if (mode == "ftomx-avc-i420gl") {
        return "video/x-h264,stream-format=avc,alignment=au "
               "! avdec_h264ftomx ! videoconvert ! video/x-raw,format=I420 "
               "! fakesink name=decoded_sink signal-handoffs=true sync=false";
    }
    if (mode == "v4l2") {
        return "v4l2h264dec ! videoconvert ! autovideosink sync=false";
    }
    if (mode == "v4l2-fake") {
        return "v4l2h264dec ! fakesink sync=false";
    }
    if (mode == "fake") {
        return "fakesink sync=false";
    }
    if (mode == "decode-fake") {
        return "decodebin ! fakesink sync=false";
    }
    return "fakesink sync=false";
}

int runListGstMode() {
    const auto factories = led::client::Decoder::availableGstreamerFactories();
    if (factories.empty()) {
        std::cout << "No GStreamer factories found or GStreamer backend is disabled.\n";
        return 0;
    }

    for (const auto& factory : factories) {
        std::cout << factory.name
                  << " rank=" << factory.rank
                  << " class=\"" << factory.klass
                  << "\" desc=\"" << factory.description << "\"\n";
    }
    return 0;
}

void printReceiverStats(const led::client::VideoReceiverStats& stats) {
    std::cout << "RTP receiver stats: packets=" << stats.packetsReceived
              << " bytes=" << stats.bytesReceived
              << " payload_bytes=" << stats.rtpPayloadBytes
              << " nals=" << stats.nalUnits
              << " idr_nals=" << stats.idrNalUnits
              << " sequence_gaps=" << stats.sequenceGaps
              << " estimated_lost=" << stats.estimatedLostPackets
              << " out_of_order=" << stats.outOfOrderPackets
              << " malformed=" << stats.malformedPackets
              << " depacketizer_drops=" << stats.depacketizerDrops
              << " last_seq=" << stats.lastSequenceNumber
              << " last_ts=" << stats.lastTimestamp << '\n';
    if (stats.timingPackets > 0) {
        std::cout << "RTP timing stats: samples=" << stats.timingPackets
                  << " min_us=" << stats.minLatencyUs
                  << " avg_us=" << stats.averageLatencyUs
                  << " max_us=" << stats.maxLatencyUs
                  << " jitter_us=" << stats.jitterUs << '\n';
    }
}

bool isReceiveTimeoutStatus(const led::Status& status) {
    return status.message().find("Resource temporarily unavailable") != std::string::npos ||
           status.message().find("timed out") != std::string::npos;
}

led::Status connectAndReady(
    const std::string& hostAddress,
    std::uint16_t controlPort,
    led::transport::TcpStream& stream,
    led::protocol::SessionOffer& offer,
    led::client::VideoReceiver& receiver) {
    auto status = stream.connect(led::transport::UdpEndpoint{hostAddress, controlPort});
    if (!status.isOk()) {
        return status;
    }

    status = stream.sendLine(led::protocol::serializeClientHello(led::client::defaultClientHello()));
    if (!status.isOk()) {
        return status;
    }

    std::string line;
    status = stream.readLine(line);
    if (!status.isOk()) {
        return status;
    }

    status = led::protocol::parseSessionOffer(line, offer);
    if (!status.isOk()) {
        return status;
    }

    status = receiver.bind(offer.rtpPort);
    if (!status.isOk()) {
        return status;
    }

    led::protocol::ClientReady ready;
    ready.rtpPort = receiver.port();
    ready.sessionToken = offer.sessionToken;
    return stream.sendLine(led::protocol::serializeClientReady(ready));
}

led::Status connectAndReceiveOffer(
    const std::string& hostAddress,
    std::uint16_t controlPort,
    led::transport::TcpStream& stream,
    led::protocol::SessionOffer& offer) {
    auto status = stream.connect(led::transport::UdpEndpoint{hostAddress, controlPort});
    if (!status.isOk()) {
        return status;
    }

    status = stream.sendLine(led::protocol::serializeClientHello(led::client::defaultClientHello()));
    if (!status.isOk()) {
        return status;
    }

    std::string line;
    status = stream.readLine(line);
    if (!status.isOk()) {
        return status;
    }

    return led::protocol::parseSessionOffer(line, offer);
}

led::Status bindReceiverAndSendReady(
    led::transport::TcpStream& stream,
    const led::protocol::SessionOffer& offer,
    led::client::VideoReceiver& receiver) {
    auto status = receiver.bind(offer.rtpPort);
    if (!status.isOk()) {
        return status;
    }

    led::protocol::ClientReady ready;
    ready.rtpPort = receiver.port();
    ready.sessionToken = offer.sessionToken;
    return stream.sendLine(led::protocol::serializeClientReady(ready));
}

int runConnectMode(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: led_client_app --connect <host-ip> [control-port]\n";
        return 1;
    }

    const std::string hostAddress = argv[2];
    const auto controlPort = argc >= 4 ? parsePort(argv[3], 17660) : std::uint16_t{17660};

    led::client::SignalingClient signalingClient;
    led::protocol::SessionOffer offer;
    auto status = signalingClient.connectAndReceiveOffer(
        hostAddress,
        controlPort,
        led::client::defaultClientHello(),
        offer);
    if (!status.isOk()) {
        led::logError(status.message());
        return 1;
    }

    led::client::VideoReceiver receiver;
    status = receiver.bind(offer.rtpPort);
    if (!status.isOk()) {
        led::logError(status.message());
        return 1;
    }
    receiver.close();

    std::cout << "Connected to host signaling " << hostAddress << ':' << controlPort << '\n';
    std::cout << "Session offer: "
              << offer.videoMode.resolution.width << 'x'
              << offer.videoMode.resolution.height << '@'
              << offer.videoMode.resolution.refreshRate
              << " codec=" << led::protocol::toString(offer.videoMode.codec)
              << " preset=" << led::protocol::toString(offer.videoMode.preset)
              << " bitrate=" << offer.videoMode.bitrateKbps
              << " rtp=" << offer.rtpPort << '\n';
    std::cout << "RTP receiver bound and closed for handshake verification\n";
    return 0;
}

int runReceiveTestNalMode(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: led_client_app --receive-test-nal <host-ip> [control-port]\n";
        return 1;
    }

    const std::string hostAddress = argv[2];
    const auto controlPort = argc >= 4 ? parsePort(argv[3], 17660) : std::uint16_t{17660};

    led::transport::TcpStream stream;
    led::protocol::SessionOffer offer;
    led::client::VideoReceiver receiver;
    auto status = connectAndReceiveOffer(hostAddress, controlPort, stream, offer);
    if (!status.isOk()) {
        led::logError(status.message());
        return 1;
    }

    receiver.setReceiveTimeoutMs(kStartupReceiveTimeoutMs);

    std::vector<std::uint8_t> nalUnit;
    led::transport::UdpEndpoint source;
    status = receiver.receiveNal(nalUnit, source);
    const auto receiverStats = receiver.stats();
    receiver.close();
    if (!status.isOk()) {
        led::logError(status.message());
        return 1;
    }

    std::cout << "Received test H.264 NAL bytes=" << nalUnit.size()
              << " from " << source.address << ':' << source.port << '\n';
    printReceiverStats(receiverStats);
    std::cout << "Offer was "
              << offer.videoMode.resolution.width << 'x'
              << offer.videoMode.resolution.height << '@'
              << offer.videoMode.resolution.refreshRate
              << " codec=" << led::protocol::toString(offer.videoMode.codec)
              << " rtp=" << offer.rtpPort << '\n';
    return 0;
}

int runReceiveTestStreamMode(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: led_client_app --receive-test-stream <host-ip> [control-port] [expected-frames] [sink] [drain-ms] [none|x11-input] [input-port]\n";
        return 1;
    }

    const std::string hostAddress = argv[2];
    const auto controlPort = argc >= 4 ? parsePort(argv[3], 17660) : std::uint16_t{17660};
    const auto expectedFrames = argc >= 5 ? parseU32(argv[4], 120) : std::uint32_t{120};
    const std::string sinkMode = argc >= 6 ? argv[5] : "fake";
    const auto drainMs = argc >= 7 ? parseU32(argv[6], 500) : std::uint32_t{500};
    const std::string inputMode = argc >= 8 ? argv[7] : "none";
    const auto inputPort = argc >= 9 ? parsePort(argv[8], 17691) : std::uint16_t{17691};

    led::transport::TcpStream stream;
    led::protocol::SessionOffer offer;
    led::client::VideoReceiver receiver;
    auto status = connectAndReceiveOffer(hostAddress, controlPort, stream, offer);
    if (!status.isOk()) {
        led::logError(status.message());
        return 1;
    }

    led::client::Renderer renderer;
    led::client::Decoder decoder;
    const bool drawWithX11Raw =
        sinkMode == "avdec-x11raw" || sinkMode == "avdec-i420gl" ||
        sinkMode == "vaapi-nv12gl" || sinkMode == "vaapi-lowlatency-nv12gl" ||
        sinkMode == "vaapi-bin-nv12gl" || sinkMode == "vaapi-avc-nv12gl" ||
        sinkMode == "omx-i420gl" || sinkMode == "ftomx-i420gl" ||
        sinkMode == "omx-avc-i420gl" || sinkMode == "ftomx-avc-i420gl";
    if (sinkMode == "vaapi-nv12gl" || sinkMode == "vaapi-lowlatency-nv12gl" ||
        sinkMode == "vaapi-bin-nv12gl" || sinkMode == "vaapi-avc-nv12gl" || sinkMode == "vaapi-sink" ||
        sinkMode == "vaapi-lowlatency-sink") {
        setenv("LIBVA_DRIVER_NAME", "r600", 0);
    }
    bool rendererOpened = false;
    decoder.setRenderer(&renderer);
    decoder.setSinkPipeline(sinkPipelineFromMode(sinkMode));
    status = decoder.configure(offer.videoMode);
    if (!status.isOk()) {
        receiver.close();
        led::logError(status.message());
        return 1;
    }
    if (drawWithX11Raw) {
        status = renderer.openFullscreen(offer.videoMode.resolution);
        if (!status.isOk()) {
            receiver.close();
            led::logError(status.message());
            return 1;
        }
        rendererOpened = true;
    }
    status = decoder.start();
    if (!status.isOk()) {
        if (rendererOpened) {
            renderer.close();
        }
        led::logError(status.message());
        return 1;
    }

    status = bindReceiverAndSendReady(stream, offer, receiver);
    if (!status.isOk()) {
        decoder.stop();
        if (rendererOpened) {
            renderer.close();
        }
        led::logError(status.message());
        return 1;
    }
    receiver.setReceiveTimeoutMs(kStartupReceiveTimeoutMs);

    std::atomic_bool stopInput{false};
    led::client::X11InputCaptureStats inputStats;
    led::Status inputStatus;
    std::thread inputThread;
    if (inputMode == "x11-input") {
        led::client::X11InputCaptureOptions inputOptions;
        inputOptions.hostAddress = hostAddress;
        inputOptions.inputPort = inputPort;
        inputOptions.hideLocalCursor = false;
        inputOptions.renderer = nullptr;
        inputThread = std::thread([&]() {
            led::client::X11InputCapture capture;
            inputStatus = capture.run(inputOptions, &stopInput, inputStats);
        });
    } else if (inputMode != "none") {
        receiver.close();
        decoder.stop();
        if (rendererOpened) {
            renderer.close();
        }
        std::cerr << "Unsupported input mode: " << inputMode << '\n';
        return 1;
    }

    std::uint32_t receivedFrames = 0;
    std::uint64_t receivedBytes = 0;
    led::transport::UdpEndpoint source;
    led::transport::UdpSocket frameAckSocket;
    led::transport::UdpEndpoint frameAckEndpoint{hostAddress, kFrameAckPort};
    auto ackStatus = frameAckSocket.open();
    if (!ackStatus.isOk()) {
        led::logWarn("frame ack telemetry disabled: " + ackStatus.message());
    }
    std::mutex frameAckMutex;
    std::uint64_t lastReceiveAckedFrameId = 0;
    std::uint64_t lastRenderAckedFrameId = 0;
    decoder.setRenderedFrameCallback([&](std::uint64_t frameId) {
        std::lock_guard<std::mutex> lock(frameAckMutex);
        sendFrameAck(frameAckSocket, frameAckEndpoint, "render", frameId, lastRenderAckedFrameId);
    });
    const auto start = std::chrono::steady_clock::now();
    auto lastStatsLog = start;
    std::uint32_t idleTimeouts = 0;
    bool activeTimeoutApplied = false;
    bool closedFromActiveIdle = false;
    std::vector<std::vector<std::uint8_t>> accessUnitNalUnits;
    std::uint64_t accessUnitFrameId = 0;
    bool accessUnitHasFrameId = false;
    auto flushAccessUnit = [&]() -> led::Status {
        if (accessUnitNalUnits.empty()) {
            return led::Status::ok();
        }
        auto flushStatus = accessUnitHasFrameId
            ? decoder.pushAccessUnitWithFrameId(accessUnitNalUnits, accessUnitFrameId)
            : decoder.pushAccessUnit(accessUnitNalUnits);
        accessUnitNalUnits.clear();
        accessUnitFrameId = 0;
        accessUnitHasFrameId = false;
        return flushStatus;
    };
    while (expectedFrames == 0 || receivedFrames < expectedFrames) {
        led::client::ReceivedNal receivedNal;
        status = receiver.receiveNal(receivedNal, source);
        if (!status.isOk()) {
            if (expectedFrames == 0 && isReceiveTimeoutStatus(status)) {
                ++idleTimeouts;
                if (receivedFrames > 0) {
                    led::logInfo("RTP stream became idle after active session; closing display session");
                    closedFromActiveIdle = true;
                    status = led::Status::ok();
                    break;
                }
                if (idleTimeouts < kStartupIdleTimeouts) {
                    led::logWarn("waiting for first RTP frame; keeping display session open");
                    continue;
                }
            }
            stopInput = true;
            if (inputThread.joinable()) {
                inputThread.join();
            }
                receiver.close();
                const auto flushStatus = flushAccessUnit();
                if (!flushStatus.isOk()) {
                    status = flushStatus;
                    led::logError(status.message());
                    return 1;
                }
                decoder.stop();
                if (rendererOpened) {
                    renderer.close();
            }
            led::logError(status.message());
            return 1;
        }
        idleTimeouts = 0;
        const auto receivedNalBytes = receivedNal.nalUnit.size();
        if (receivedNal.hasFrameId) {
            std::lock_guard<std::mutex> lock(frameAckMutex);
            sendFrameAck(frameAckSocket, frameAckEndpoint, "receive", receivedNal.frameId, lastReceiveAckedFrameId);
        }
        if (receivedNal.hasFrameId && accessUnitHasFrameId && accessUnitFrameId != receivedNal.frameId) {
            status = flushAccessUnit();
        }
        if (status.isOk()) {
            if (receivedNal.hasFrameId && accessUnitNalUnits.empty()) {
                accessUnitFrameId = receivedNal.frameId;
                accessUnitHasFrameId = true;
            }
            accessUnitNalUnits.push_back(std::move(receivedNal.nalUnit));
            if (receivedNal.endOfFrame) {
                status = flushAccessUnit();
            }
        }
        if (!status.isOk()) {
            stopInput = true;
            if (inputThread.joinable()) {
                inputThread.join();
            }
            receiver.close();
            decoder.stop();
            if (rendererOpened) {
                renderer.close();
            }
            led::logError(status.message());
            return 1;
        }
        if (!activeTimeoutApplied) {
            receiver.setReceiveTimeoutMs(kActiveReceiveTimeoutMs);
            activeTimeoutApplied = true;
        }
        ++receivedFrames;
        receivedBytes += receivedNalBytes;

        const auto now = std::chrono::steady_clock::now();
        if (now - lastStatsLog >= std::chrono::seconds(1)) {
            const auto receiverLiveStats = receiver.stats();
            const auto decoderLiveStats = decoder.stats();
            const auto rendererLiveStats = renderer.stats();
            std::cout << "Client video live stats: frames=" << receivedFrames
                      << " rtp_packets=" << receiverLiveStats.packetsReceived
                      << " rtp_avg_ms=" << (receiverLiveStats.averageLatencyUs / 1000.0)
                      << " rtp_jitter_ms=" << (receiverLiveStats.jitterUs / 1000.0)
                      << " decoded=" << decoderLiveStats.decodedFrames
                      << " rendered=" << rendererLiveStats.rawFrames
                      << " drops=" << receiverLiveStats.depacketizerDrops
                      << " sequence_gaps=" << receiverLiveStats.sequenceGaps
                      << " last_frame_id=" << receiverLiveStats.lastFrameId << '\n';
            lastStatsLog = now;
        }
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const auto receiverStats = receiver.stats();
    receiver.close();
    status = flushAccessUnit();
    if (!status.isOk()) {
        decoder.stop();
        if (rendererOpened) {
            renderer.close();
        }
        led::logError(status.message());
        return 1;
    }
    decoder.drainForMs(closedFromActiveIdle ? std::uint32_t{100} : drainMs);
    stopInput = true;
    if (inputThread.joinable()) {
        inputThread.join();
    }
    const auto decoderStats = decoder.stats();
    const auto rendererStats = renderer.stats();
    decoder.stop();
    frameAckSocket.close();
    if (rendererOpened) {
        renderer.close();
    }

    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    const auto fps = elapsedMs > 0 ? (static_cast<double>(receivedFrames) * 1000.0 / static_cast<double>(elapsedMs)) : 0.0;

    std::cout << "Received test stream frames=" << receivedFrames
              << " bytes=" << receivedBytes
              << " elapsed_ms=" << elapsedMs
              << " fps=" << fps
              << " from " << source.address << ':' << source.port << '\n';
    printReceiverStats(receiverStats);
    std::cout << "Decoder sink stats: nal_units=" << decoderStats.nalUnits
              << " bytes=" << decoderStats.bytes
              << " keyframes=" << decoderStats.keyFrames
              << " decoded_frames=" << decoderStats.decodedFrames
              << " backend=" << decoder.backendName()
              << " sink_mode=" << sinkMode
              << " backend_accepted=" << decoderStats.backendAccepted << '\n';
    std::cout << "Renderer stats: raw_frames=" << rendererStats.rawFrames
              << " bytes=" << rendererStats.bytes
              << " last=" << rendererStats.lastFrame.width << 'x'
              << rendererStats.lastFrame.height
              << " format=" << rendererStats.lastFrame.format
              << " last_bytes=" << rendererStats.lastFrame.bytes << '\n';
    if (inputMode == "x11-input") {
        std::cout << "X11 input stats: events_sent=" << inputStats.eventsSent
                  << " moves=" << inputStats.pointerMoves
                  << " buttons=" << inputStats.pointerButtons
                  << " wheels=" << inputStats.wheelEvents
                  << " keys=" << inputStats.keyEvents
                  << " ignored=" << inputStats.ignoredEvents
                  << " status=" << (inputStatus.isOk() ? "ok" : inputStatus.message()) << '\n';
    }
    std::cout << "Offer was "
              << offer.videoMode.resolution.width << 'x'
              << offer.videoMode.resolution.height << '@'
              << offer.videoMode.resolution.refreshRate
              << " codec=" << led::protocol::toString(offer.videoMode.codec)
              << " rtp=" << offer.rtpPort << '\n';
    return 0;
}

int runCaptureX11InputMode(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: led_client_app --capture-x11-input <host-ip> [input-port] [max-events] [idle-timeout-ms]\n";
        return 1;
    }

    led::client::X11InputCaptureOptions options;
    options.hostAddress = argv[2];
    options.inputPort = argc >= 4 ? parsePort(argv[3], 17691) : std::uint16_t{17691};
    options.maxEvents = argc >= 5 ? parseU32(argv[4], 0) : std::uint32_t{0};
    options.idleTimeoutMs = argc >= 6 ? parseU32(argv[5], 0) : std::uint32_t{0};

    led::client::X11InputCaptureStats stats;
    led::client::X11InputCapture capture;
    const auto status = capture.run(options, nullptr, stats);
    if (!status.isOk()) {
        led::logError(status.message());
        return 1;
    }

    std::cout << "Captured X11 input events_sent=" << stats.eventsSent
              << " moves=" << stats.pointerMoves
              << " buttons=" << stats.pointerButtons
              << " wheels=" << stats.wheelEvents
              << " keys=" << stats.keyEvents
              << " ignored=" << stats.ignoredEvents
              << " to " << options.hostAddress << ':' << options.inputPort << '\n';
    return 0;
}

int runDecodeH264FileMode(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: led_client_app --decode-h264-file <file.h264> [sink] [hold-ms]\n";
        return 1;
    }

    const char* path = argv[2];
    const std::string sinkMode = argc >= 4 ? argv[3] : "fake";
    const auto holdMs = argc >= 5 ? parseU32(argv[4], 500) : std::uint32_t{500};

    std::vector<std::uint8_t> fileBytes;
    auto status = led::transport::readBinaryFile(path, fileBytes);
    if (!status.isOk()) {
        led::logError(status.message());
        return 1;
    }

    const auto nalUnits = led::transport::splitAnnexB(fileBytes);
    if (nalUnits.empty()) {
        led::logError("no Annex B H.264 NAL units found in input file");
        return 1;
    }

    auto mode = led::protocol::defaultStandardMode();
    led::client::Renderer renderer;
    led::client::Decoder decoder;
    decoder.setRenderer(&renderer);
    decoder.setSinkPipeline(sinkPipelineFromMode(sinkMode));
    status = decoder.configure(mode);
    if (!status.isOk()) {
        led::logError(status.message());
        return 1;
    }

    status = decoder.start();
    if (!status.isOk()) {
        led::logError(status.message());
        return 1;
    }

    std::uint64_t pushedBytes = 0;
    for (const auto& nal : nalUnits) {
        status = decoder.pushNal(nal.bytes);
        if (!status.isOk()) {
            decoder.stop();
            led::logError(status.message());
            return 1;
        }
        pushedBytes += nal.bytes.size();
    }

    decoder.drainForMs(holdMs);
    const auto stats = decoder.stats();
    const auto rendererStats = renderer.stats();
    decoder.stop();

    std::cout << "Decoded file NAL units=" << nalUnits.size()
              << " pushed_bytes=" << pushedBytes << '\n';
    std::cout << "Decoder sink stats: nal_units=" << stats.nalUnits
              << " bytes=" << stats.bytes
              << " keyframes=" << stats.keyFrames
              << " decoded_frames=" << stats.decodedFrames
              << " backend=" << decoder.backendName()
              << " sink_mode=" << sinkMode
              << " backend_accepted=" << stats.backendAccepted << '\n';
    std::cout << "Renderer stats: raw_frames=" << rendererStats.rawFrames
              << " bytes=" << rendererStats.bytes
              << " last=" << rendererStats.lastFrame.width << 'x'
              << rendererStats.lastFrame.height
              << " format=" << rendererStats.lastFrame.format
              << " last_bytes=" << rendererStats.lastFrame.bytes << '\n';
    return 0;
}

int runReceiveMjpegStreamMode(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: led_client_app --receive-mjpeg-stream <host-ip> [control-port] [expected-frames] [drain-ms] [none|x11-input] [input-port]\n";
        return 1;
    }

    const std::string hostAddress = argv[2];
    const auto controlPort = argc >= 4 ? parsePort(argv[3], 17660) : std::uint16_t{17660};
    const auto expectedFrames = argc >= 5 ? parseU32(argv[4], 0) : std::uint32_t{0};
    const auto drainMs = argc >= 6 ? parseU32(argv[5], 100) : std::uint32_t{100};
    const std::string inputMode = argc >= 7 ? argv[6] : "x11-input";
    const auto inputPort = argc >= 8 ? parsePort(argv[7], 17691) : std::uint16_t{17691};

    led::transport::TcpStream stream;
    led::protocol::SessionOffer offer;
    auto status = connectAndReceiveOffer(hostAddress, controlPort, stream, offer);
    if (!status.isOk()) {
        led::logError(status.message());
        return 1;
    }

    led::transport::UdpSocket socket;
    status = socket.bind(offer.rtpPort);
    if (!status.isOk()) {
        led::logError(status.message());
        return 1;
    }
    socket.setReceiveTimeoutMs(kStartupReceiveTimeoutMs);

    led::protocol::ClientReady ready;
    ready.rtpPort = socket.localPort();
    ready.sessionToken = offer.sessionToken;
    status = stream.sendLine(led::protocol::serializeClientReady(ready));
    if (!status.isOk()) {
        socket.close();
        led::logError(status.message());
        return 1;
    }

    led::client::Renderer renderer;
    led::client::Decoder decoder;
#if !defined(LED_HAS_LIBJPEG)
    decoder.setInputPipeline("image/jpeg", "");
    decoder.setSinkPipeline("jpegdec ! videoconvert ! video/x-raw,format=BGRx ! fakesink name=decoded_sink signal-handoffs=true sync=false");
    decoder.setRenderer(&renderer);
    status = decoder.configure(offer.videoMode);
    if (!status.isOk()) {
        socket.close();
        led::logError(status.message());
        return 1;
    }
#endif
    status = renderer.openFullscreen(offer.videoMode.resolution);
    if (!status.isOk()) {
        socket.close();
        led::logError(status.message());
        return 1;
    }
#if !defined(LED_HAS_LIBJPEG)
    status = decoder.start();
    if (!status.isOk()) {
        renderer.close();
        socket.close();
        led::logError(status.message());
        return 1;
    }
#else
    led::logInfo("client MJPEG decoder using libjpeg-turbo direct BGRx path");
#endif

    std::atomic_bool stopInput{false};
    led::client::X11InputCaptureStats inputStats;
    led::Status inputStatus;
    std::thread inputThread;
    if (inputMode == "x11-input") {
        led::client::X11InputCaptureOptions inputOptions;
        inputOptions.hostAddress = hostAddress;
        inputOptions.inputPort = inputPort;
        inputOptions.hideLocalCursor = false;
        inputOptions.renderer = &renderer;
        inputThread = std::thread([&]() {
            led::client::X11InputCapture capture;
            inputStatus = capture.run(inputOptions, &stopInput, inputStats);
        });
    } else if (inputMode != "none") {
#if !defined(LED_HAS_LIBJPEG)
        decoder.stop();
#endif
        renderer.close();
        socket.close();
        std::cerr << "Unsupported input mode: " << inputMode << '\n';
        return 1;
    }

    led::transport::UdpSocket frameAckSocket;
    led::transport::UdpEndpoint frameAckEndpoint{hostAddress, kFrameAckPort};
    auto ackStatus = frameAckSocket.open();
    if (!ackStatus.isOk()) {
        led::logWarn("frame ack telemetry disabled: " + ackStatus.message());
    }
    std::mutex frameAckMutex;
    std::uint64_t lastReceiveAckedFrameId = 0;
    std::uint64_t lastRenderAckedFrameId = 0;
    std::atomic<std::uint64_t> lastSubmittedFrameId{0};
    std::atomic<std::uint64_t> lastRenderedFrameId{0};
    decoder.setRenderedFrameCallback([&](std::uint64_t frameId) {
        lastRenderedFrameId.store(frameId, std::memory_order_release);
        std::lock_guard<std::mutex> lock(frameAckMutex);
        sendFrameAck(frameAckSocket, frameAckEndpoint, "render", frameId, lastRenderAckedFrameId);
    });

    led::transport::MjpegFrameAssembler assembler;
    led::transport::UdpEndpoint source;
    std::vector<std::uint8_t> datagramBytes;
    std::uint32_t frames = 0;
    std::uint64_t packets = 0;
    std::uint64_t malformed = 0;
    std::uint64_t bytes = 0;
    std::uint64_t decodeDropped = 0;
    std::atomic<std::uint64_t> directDecodedFrames{0};
    std::uint32_t idleTimeouts = 0;
    bool activeTimeoutApplied = false;
    bool closedFromActiveIdle = false;
    const auto start = std::chrono::steady_clock::now();
    auto lastStatsLog = start;

#if defined(LED_HAS_LIBJPEG)
    std::mutex latestFrameMutex;
    std::condition_variable latestFrameCv;
    led::transport::ReassembledMjpegFrame latestFrame;
    bool latestFrameAvailable = false;
    bool latestFrameStop = false;
    led::Status directDecodeStatus = led::Status::ok();
    std::thread directDecodeThread([&]() {
        std::vector<std::uint8_t> pixels;
        bool receivedKeyFrame = false;
        while (true) {
            led::transport::ReassembledMjpegFrame frameToDecode;
            {
                std::unique_lock<std::mutex> lock(latestFrameMutex);
                latestFrameCv.wait(lock, [&]() { return latestFrameStop || latestFrameAvailable; });
                if (latestFrameStop && !latestFrameAvailable) {
                    break;
                }
                frameToDecode = std::move(latestFrame);
                latestFrameAvailable = false;
            }
            const auto canvasWidth = frameToDecode.canvasWidth != 0
                ? frameToDecode.canvasWidth
                : offer.videoMode.resolution.width;
            const auto canvasHeight = frameToDecode.canvasHeight != 0
                ? frameToDecode.canvasHeight
                : offer.videoMode.resolution.height;
            const auto rectWidth = frameToDecode.rectWidth != 0 ? frameToDecode.rectWidth : canvasWidth;
            const auto rectHeight = frameToDecode.rectHeight != 0 ? frameToDecode.rectHeight : canvasHeight;
            const bool keyFrame = frameToDecode.keyFrame ||
                (frameToDecode.rectX == 0 && frameToDecode.rectY == 0 &&
                 rectWidth == canvasWidth && rectHeight == canvasHeight);
            if (!keyFrame && !receivedKeyFrame) {
                continue;
            }

            led::client::RawFrameInfo rawFrame;
            auto decodeStatus = decodeJpegToBgrx(frameToDecode.jpegBytes, frameToDecode.frameId, rawFrame, pixels);
            if (!decodeStatus.isOk()) {
                directDecodeStatus = decodeStatus;
                break;
            }
            if (rawFrame.width != rectWidth || rawFrame.height != rectHeight) {
                directDecodeStatus = led::Status::internalError("decoded MJPEG rect size does not match packet metadata");
                break;
            }
            if (keyFrame) {
                decodeStatus = renderer.submitRawFrameData(rawFrame, pixels.data());
                receivedKeyFrame = decodeStatus.isOk();
            } else {
                decodeStatus = renderer.submitRawFrameRegion(rawFrame, pixels.data(), frameToDecode.rectX, frameToDecode.rectY);
            }
            if (!decodeStatus.isOk()) {
                directDecodeStatus = decodeStatus;
                break;
            }
            directDecodedFrames.fetch_add(1, std::memory_order_relaxed);
            {
                std::lock_guard<std::mutex> lock(frameAckMutex);
                sendFrameAck(frameAckSocket, frameAckEndpoint, "render", frameToDecode.frameId, lastRenderAckedFrameId);
            }
        }
    });
#endif

    while (expectedFrames == 0 || frames < expectedFrames) {
        status = socket.receiveFrom(65535, datagramBytes, source);
        if (!status.isOk()) {
            if (expectedFrames == 0) {
                ++idleTimeouts;
                if (frames > 0) {
                    if (idleTimeouts < 6) {
                        status = led::Status::ok();
                        continue;
                    }
                    closedFromActiveIdle = true;
                    status = led::Status::ok();
                    break;
                }
                if (idleTimeouts < kStartupIdleTimeouts) {
                    led::logWarn("waiting for first MJPEG frame; keeping display session open");
                    continue;
                }
            }
            break;
        }
        idleTimeouts = 0;
        ++packets;

        led::transport::MjpegDatagram datagram;
        status = led::transport::parseMjpegDatagram(datagramBytes, datagram);
        if (!status.isOk()) {
            ++malformed;
            status = led::Status::ok();
            continue;
        }
        led::transport::ReassembledMjpegFrame frame;
        status = assembler.pushDatagram(datagram, frame);
        if (!status.isOk()) {
            ++malformed;
            status = led::Status::ok();
            continue;
        }
        if (frame.jpegBytes.empty()) {
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(frameAckMutex);
            sendFrameAck(frameAckSocket, frameAckEndpoint, "receive", frame.frameId, lastReceiveAckedFrameId);
        }
        if (!activeTimeoutApplied) {
            socket.setReceiveTimeoutMs(kActiveReceiveTimeoutMs);
            activeTimeoutApplied = true;
        }
        ++frames;
        bytes += frame.jpegBytes.size();

#if defined(LED_HAS_LIBJPEG)
        {
            std::lock_guard<std::mutex> lock(latestFrameMutex);
            if (latestFrameAvailable) {
                ++decodeDropped;
            }
            latestFrame = std::move(frame);
            latestFrameAvailable = true;
        }
        latestFrameCv.notify_one();
#else
        const auto submittedFrameId = lastSubmittedFrameId.load(std::memory_order_acquire);
        const auto renderedFrameId = lastRenderedFrameId.load(std::memory_order_acquire);
        if (submittedFrameId != 0 && renderedFrameId < submittedFrameId) {
            ++decodeDropped;
        } else {
            status = decoder.pushEncodedBufferWithFrameId(frame.jpegBytes, frame.frameId);
            if (!status.isOk()) {
                break;
            }
            lastSubmittedFrameId.store(frame.frameId, std::memory_order_release);
        }
#endif

        const auto now = std::chrono::steady_clock::now();
        if (now - lastStatsLog >= std::chrono::seconds(1)) {
#if defined(LED_HAS_LIBJPEG)
            const auto rendererLiveStats = renderer.stats();
            const auto decodedFrames = directDecodedFrames.load(std::memory_order_relaxed);
#else
            const auto decoderLiveStats = decoder.stats();
            const auto rendererLiveStats = renderer.stats();
            const auto decodedFrames = decoderLiveStats.decodedFrames;
#endif
            std::cout << "Client MJPEG live stats: frames=" << frames
                      << " packets=" << packets
                        << " malformed=" << malformed
                        << " decode_dropped=" << decodeDropped
                        << " decoded=" << decodedFrames
                        << " rendered=" << rendererLiveStats.rawFrames
                        << " avg_frame_kb=" << (frames == 0 ? 0 : (bytes / 1024 / frames))
                      << " last_frame_id=" << frame.frameId << '\n';
            lastStatsLog = now;
        }
    }

    const auto elapsed = std::chrono::steady_clock::now() - start;
    socket.close();
#if defined(LED_HAS_LIBJPEG)
    {
        std::lock_guard<std::mutex> lock(latestFrameMutex);
        latestFrameAvailable = false;
        latestFrameStop = true;
    }
    latestFrameCv.notify_one();
    if (directDecodeThread.joinable()) {
        directDecodeThread.join();
    }
    if (status.isOk() && !directDecodeStatus.isOk()) {
        status = directDecodeStatus;
    }
    (void)closedFromActiveIdle;
    (void)drainMs;
    const auto decoderStats = decoder.stats();
#else
    decoder.drainForMs(closedFromActiveIdle ? std::uint32_t{100} : drainMs);
    const auto decoderStats = decoder.stats();
#endif
    const auto rendererStats = renderer.stats();
#if !defined(LED_HAS_LIBJPEG)
    decoder.stop();
#endif
    stopInput = true;
    if (inputThread.joinable()) {
        inputThread.join();
    }
    if (status.isOk() && !inputStatus.isOk()) {
        status = inputStatus;
    }
    frameAckSocket.close();
    renderer.close();
    if (!status.isOk()) {
        led::logError(status.message());
        return 1;
    }

    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    const auto fps = elapsedMs > 0 ? (static_cast<double>(frames) * 1000.0 / static_cast<double>(elapsedMs)) : 0.0;
    std::cout << "Received MJPEG stream frames=" << frames
                << " packets=" << packets
                << " malformed=" << malformed
                << " decode_dropped=" << decodeDropped
                << " bytes=" << bytes
              << " elapsed_ms=" << elapsedMs
              << " fps=" << fps
              << " from " << source.address << ':' << source.port << '\n';
    std::cout << "Decoder sink stats: buffers=" << decoderStats.nalUnits
              << " bytes=" << decoderStats.bytes
              << " decoded_frames=" << decoderStats.decodedFrames
              << " backend=" << decoder.backendName()
              << " backend_accepted=" << decoderStats.backendAccepted << '\n';
    std::cout << "Renderer stats: raw_frames=" << rendererStats.rawFrames
              << " bytes=" << rendererStats.bytes
              << " last=" << rendererStats.lastFrame.width << 'x'
              << rendererStats.lastFrame.height
              << " format=" << rendererStats.lastFrame.format
              << " last_bytes=" << rendererStats.lastFrame.bytes << '\n';
    return 0;
}

int runSendTestInputMode(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: led_client_app --send-test-input <host-ip> [input-port] [count] [interval-ms] [move|mouse|keyboard|mixed]\n";
        return 1;
    }

    const std::string hostAddress = argv[2];
    const auto inputPort = argc >= 4 ? parsePort(argv[3], 17691) : std::uint16_t{17691};
    const auto count = argc >= 5 ? parseU32(argv[4], 10) : std::uint32_t{10};
    const auto intervalMs = argc >= 6 ? parseU32(argv[5], 8) : std::uint32_t{8};
    const std::string pattern = argc >= 7 ? argv[6] : "move";

    led::client::InputSender sender;
    auto status = sender.open(hostAddress, inputPort);
    if (!status.isOk()) {
        led::logError(status.message());
        return 1;
    }

    led::client::InputCapture capture;
    std::vector<led::protocol::InputEvent> events;
    events.reserve(count + 8);
    if (pattern == "move") {
        for (std::uint32_t index = 0; index < count; ++index) {
            const auto denominator = count > 1 ? static_cast<double>(count - 1) : 1.0;
            const auto x = static_cast<double>(index) / denominator;
            events.push_back(capture.pointerMove(x, 0.5));
        }
    } else if (pattern == "mouse") {
        if (count == 1) {
            events.push_back(capture.pointerMove(0.5, 0.5));
        } else if (count >= 2) {
            const auto moves = count - 2;
            for (std::uint32_t index = 0; index < moves; ++index) {
                const auto denominator = moves > 1 ? static_cast<double>(moves - 1) : 1.0;
                const auto x = static_cast<double>(index) / denominator;
                events.push_back(capture.pointerMove(x, 0.5));
            }
            events.push_back(capture.pointerButton(0.5, 0.5, 1, true));
            events.push_back(capture.pointerButton(0.5, 0.5, 1, false));
        }
    } else if (pattern == "keyboard") {
        while (events.size() + 2 <= count) {
            events.push_back(capture.key(0x41, true));
            events.push_back(capture.key(0x41, false));
        }
        if (events.size() < count) {
            events.push_back(capture.pointerMove(0.5, 0.5));
        }
    } else if (pattern == "mixed") {
        events.push_back(capture.pointerMove(0.25, 0.5));
        events.push_back(capture.pointerMove(0.75, 0.5));
        events.push_back(capture.pointerButton(0.75, 0.5, 1, true));
        events.push_back(capture.pointerButton(0.75, 0.5, 1, false));
        events.push_back(capture.wheel(120));
        events.push_back(capture.key(0x41, true));
        events.push_back(capture.key(0x41, false));
        while (events.size() < count) {
            const auto x = events.size() % 2 == 0 ? 0.25 : 0.75;
            events.push_back(capture.pointerMove(x, 0.5));
        }
        if (events.size() > count) {
            events.resize(count);
        }
    } else {
        sender.close();
        std::cerr << "Unsupported input pattern: " << pattern << '\n';
        return 1;
    }

    for (const auto& event : events) {
        status = sender.send(event);
        if (!status.isOk()) {
            sender.close();
            led::logError(status.message());
            return 1;
        }
        if (intervalMs > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
        }
    }

    sender.close();
    std::cout << "Sent input events count=" << events.size()
              << " to " << hostAddress << ':' << inputPort
              << " interval_ms=" << intervalMs
              << " pattern=" << pattern << '\n';
    return 0;
}

int runSkeletonMode() {
    led::logInfo("LAN extended display Linux ARM64 client skeleton starting");

    led::client::Discovery discovery;
    const auto hosts = discovery.scanHosts();
    if (hosts.empty()) {
        led::logWarn("no Windows host discovered");
        return 1;
    }

    led::client::SessionClient sessionClient;
    const auto mode = led::protocol::defaultStandardMode();
    const auto status = sessionClient.connectToHost(hosts.front(), mode);
    if (!status.isOk()) {
        led::logError(status.message());
        return 1;
    }

    led::client::InputCapture inputCapture;
    const auto move = inputCapture.pointerMove(0.5, 0.5);

    std::cout << "Client skeleton state: " << led::session::toString(sessionClient.state()) << '\n';
    std::cout << "Connected to: " << hosts.front().name << '\n';
    std::cout << "Local cursor event: " << led::protocol::toString(move.kind)
              << " seq=" << move.sequence << '\n';

    sessionClient.disconnect();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::cout.setf(std::ios::unitbuf);
    std::cerr.setf(std::ios::unitbuf);

    if (argc >= 2 && std::string(argv[1]) == "--list-gst") {
        return runListGstMode();
    }
    if (argc >= 2 && std::string(argv[1]) == "--connect") {
        return runConnectMode(argc, argv);
    }
    if (argc >= 2 && std::string(argv[1]) == "--receive-test-nal") {
        return runReceiveTestNalMode(argc, argv);
    }
    if (argc >= 2 && std::string(argv[1]) == "--receive-test-stream") {
        return runReceiveTestStreamMode(argc, argv);
    }
    if (argc >= 2 && std::string(argv[1]) == "--receive-mjpeg-stream") {
        return runReceiveMjpegStreamMode(argc, argv);
    }
    if (argc >= 2 && std::string(argv[1]) == "--decode-h264-file") {
        return runDecodeH264FileMode(argc, argv);
    }
    if (argc >= 2 && std::string(argv[1]) == "--send-test-input") {
        return runSendTestInputMode(argc, argv);
    }
    if (argc >= 2 && std::string(argv[1]) == "--capture-x11-input") {
        return runCaptureX11InputMode(argc, argv);
    }

    return runSkeletonMode();
}
