#include "led/common/logger.h"
#include "led/host/display_manager.h"
#include "led/host/cursor_suppressor.h"
#include "led/host/input_injector.h"
#include "led/host/input_receiver.h"
#include "led/host/jpeg_encoder.h"
#include "led/host/signaling_server.h"
#include "led/host/session_manager.h"
#include "led/host/video_sender.h"
#include "led/protocol/control_messages.h"
#include "led/protocol/messages.h"
#include "led/session/session_state.h"
#include "led/transport/h264_annex_b.h"
#include "led/transport/mjpeg_packet.h"
#include "led/transport/tcp_socket.h"
#include "led/transport/udp_socket.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

constexpr const char* kLiveCaptureStopEventName = "Local\\LanExtendedDisplayHostStop";
constexpr std::uint16_t kFrameAckPort = 17692;
#if defined(_WIN32)
constexpr const wchar_t* kDriverCreateMonitorEventName = L"Global\\LanExtendedDisplayCreateMonitor";
#endif

bool signalDriverCreateMonitor() {
#if defined(_WIN32)
    HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, kDriverCreateMonitorEventName);
    if (event == nullptr) {
        return false;
    }
    const BOOL ok = SetEvent(event);
    CloseHandle(event);
    return ok != FALSE;
#else
    return false;
#endif
}

struct FrameAckStats {
    std::uint64_t acks{0};
    std::uint64_t unknownAcks{0};
    double averageRttUs{0.0};
    std::uint64_t maxRttUs{0};
    std::uint64_t lastFrameId{0};
};

enum class FrameAckStage {
    receive,
    render,
};

struct ParsedFrameAck {
    std::uint64_t frameId{0};
    FrameAckStage stage{FrameAckStage::receive};
};

struct FrameAckTracker {
    std::mutex mutex;
    std::unordered_map<std::uint64_t, std::chrono::steady_clock::time_point> sentFrames;
    FrameAckStats receiveStats;
    FrameAckStats renderStats;
};

std::string parseAckField(const std::string& message, const char* key) {
    const auto begin = message.find(key);
    if (begin == std::string::npos) {
        return {};
    }
    const auto valueBegin = begin + std::char_traits<char>::length(key);
    const auto end = message.find(';', valueBegin);
    return message.substr(valueBegin, end == std::string::npos ? std::string::npos : end - valueBegin);
}

std::optional<ParsedFrameAck> parseFrameAck(const std::string& message) {
    constexpr const char* prefix = "LED_FRAME_ACK_V1";
    if (message.rfind(prefix, 0) != 0) {
        return std::nullopt;
    }
    const auto frameText = parseAckField(message, "frame=");
    if (frameText.empty()) {
        return std::nullopt;
    }
    try {
        ParsedFrameAck ack;
        ack.frameId = static_cast<std::uint64_t>(std::stoull(frameText));
        ack.stage = parseAckField(message, "stage=") == "render" ? FrameAckStage::render : FrameAckStage::receive;
        return ack;
    } catch (...) {
        return std::nullopt;
    }
}

void noteFrameSent(FrameAckTracker& tracker, std::uint64_t frameId) {
    if (frameId == 0) {
        return;
    }
    std::scoped_lock lock(tracker.mutex);
    tracker.sentFrames[frameId] = std::chrono::steady_clock::now();
    if (tracker.sentFrames.size() > 240) {
        const auto cutoff = frameId > 240 ? frameId - 240 : 0;
        for (auto it = tracker.sentFrames.begin(); it != tracker.sentFrames.end();) {
            if (it->first < cutoff) {
                it = tracker.sentFrames.erase(it);
            } else {
                ++it;
            }
        }
    }
}

void updateAckStats(FrameAckStats& stats, std::uint64_t frameId, std::uint64_t rttUs) {
    ++stats.acks;
    stats.lastFrameId = std::max(stats.lastFrameId, frameId);
    stats.maxRttUs = std::max(stats.maxRttUs, rttUs);
    if (stats.acks == 1) {
        stats.averageRttUs = static_cast<double>(rttUs);
    } else {
        stats.averageRttUs += (static_cast<double>(rttUs) - stats.averageRttUs) /
            static_cast<double>(stats.acks);
    }
}

void noteFrameAck(FrameAckTracker& tracker, const ParsedFrameAck& ack) {
    const auto now = std::chrono::steady_clock::now();
    std::scoped_lock lock(tracker.mutex);
    auto found = tracker.sentFrames.find(ack.frameId);
    auto& stats = ack.stage == FrameAckStage::render ? tracker.renderStats : tracker.receiveStats;
    if (found == tracker.sentFrames.end()) {
        ++stats.unknownAcks;
        return;
    }
    const auto rttUs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now - found->second).count());
    updateAckStats(stats, ack.frameId, rttUs);
    if (ack.stage == FrameAckStage::render) {
        tracker.sentFrames.erase(found);
    }
}

std::pair<FrameAckStats, FrameAckStats> snapshotFrameAckStats(FrameAckTracker& tracker) {
    std::scoped_lock lock(tracker.mutex);
    return {tracker.receiveStats, tracker.renderStats};
}

struct DirtyRect {
    std::uint32_t x{0};
    std::uint32_t y{0};
    std::uint32_t width{0};
    std::uint32_t height{0};
    bool fullFrame{true};
    bool empty{false};
};

DirtyRect fullDirtyRect(std::uint32_t width, std::uint32_t height) {
    return DirtyRect{0, 0, width, height, true, width == 0 || height == 0};
}

DirtyRect computeDirtyRect(
    const led::host::CapturedFrame& current,
    const led::host::CapturedFrame& previous,
    std::uint32_t alignPixels,
    double fullFrameAreaRatio) {
    if (current.bgra.empty() || current.width == 0 || current.height == 0) {
        return DirtyRect{0, 0, 0, 0, true, true};
    }
    if (previous.bgra.size() != current.bgra.size() ||
        previous.width != current.width ||
        previous.height != current.height) {
        return fullDirtyRect(current.width, current.height);
    }

    std::uint32_t minX = current.width;
    std::uint32_t minY = current.height;
    std::uint32_t maxX = 0;
    std::uint32_t maxY = 0;
    bool changed = false;
    for (std::uint32_t y = 0; y < current.height; ++y) {
        const auto rowOffset = static_cast<std::size_t>(y) * current.width * 4;
        for (std::uint32_t x = 0; x < current.width; ++x) {
            const auto offset = rowOffset + static_cast<std::size_t>(x) * 4;
            if (current.bgra[offset] != previous.bgra[offset] ||
                current.bgra[offset + 1] != previous.bgra[offset + 1] ||
                current.bgra[offset + 2] != previous.bgra[offset + 2]) {
                minX = std::min(minX, x);
                minY = std::min(minY, y);
                maxX = std::max(maxX, x);
                maxY = std::max(maxY, y);
                changed = true;
            }
        }
    }
    if (!changed) {
        return DirtyRect{0, 0, 0, 0, false, true};
    }

    const auto align = std::max<std::uint32_t>(1, alignPixels);
    minX = (minX / align) * align;
    minY = (minY / align) * align;
    maxX = std::min<std::uint32_t>(current.width - 1, ((maxX + align) / align) * align + (align - 1));
    maxY = std::min<std::uint32_t>(current.height - 1, ((maxY + align) / align) * align + (align - 1));
    const auto width = maxX - minX + 1;
    const auto height = maxY - minY + 1;
    const auto area = static_cast<double>(width) * static_cast<double>(height);
    const auto fullArea = static_cast<double>(current.width) * static_cast<double>(current.height);
    if (fullArea <= 0.0 || area / fullArea >= fullFrameAreaRatio) {
        return fullDirtyRect(current.width, current.height);
    }
    return DirtyRect{minX, minY, width, height, false, false};
}

std::thread startFrameAckListener(FrameAckTracker& tracker, std::atomic_bool& stop) {
    return std::thread([&tracker, &stop]() {
        led::transport::UdpSocket socket;
        auto status = socket.bind(kFrameAckPort);
        if (!status.isOk()) {
            led::logWarn("frame ack listener disabled: " + status.message());
            return;
        }
        socket.setReceiveTimeoutMs(250);
        led::logInfo("host frame ack listener bound on UDP port " + std::to_string(kFrameAckPort));
        while (!stop.load()) {
            std::vector<std::uint8_t> bytes;
            led::transport::UdpEndpoint source;
            status = socket.receiveFrom(512, bytes, source);
            if (!status.isOk()) {
                continue;
            }
            const std::string message(bytes.begin(), bytes.end());
            const auto ack = parseFrameAck(message);
            if (ack.has_value()) {
                noteFrameAck(tracker, *ack);
            }
        }
        socket.close();
    });
}

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

#if defined(_WIN32)
struct LiveCaptureStopEvent {
    HANDLE handle{nullptr};

    LiveCaptureStopEvent() {
        handle = CreateEventA(nullptr, TRUE, FALSE, kLiveCaptureStopEventName);
        if (handle != nullptr) {
            ResetEvent(handle);
        }
    }

    ~LiveCaptureStopEvent() {
        if (handle != nullptr) {
            CloseHandle(handle);
        }
    }

    bool requested() const {
        return handle != nullptr && WaitForSingleObject(handle, 0) == WAIT_OBJECT_0;
    }
};

void enableProcessDpiAwareness() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
}
#else
struct LiveCaptureStopEvent {
    bool requested() const {
        return false;
    }
};

void enableProcessDpiAwareness() {}
#endif

std::uint32_t parseU32(const char* value, std::uint32_t fallback) {
    try {
        return static_cast<std::uint32_t>(std::stoul(value));
    } catch (...) {
    }
    return fallback;
}

led::protocol::VideoMode makeVideoMode(std::uint32_t width, std::uint32_t height, std::uint32_t fps, std::uint32_t bitrateKbps) {
    auto mode = led::protocol::defaultStandardMode();
    if (width > 0) {
        mode.resolution.width = width;
    }
    if (height > 0) {
        mode.resolution.height = height;
    }
    if (fps > 0) {
        mode.resolution.refreshRate = fps;
    }
    if (bitrateKbps > 0) {
        mode.bitrateKbps = bitrateKbps;
    }
    return mode;
}

void printInputReceiverStats(const led::host::InputReceiverStats& stats) {
    std::cout << "Input receiver stats: packets=" << stats.packetsReceived
              << " bytes=" << stats.bytesReceived
              << " malformed=" << stats.malformedPackets
              << " events=" << stats.eventsReceived
              << " moves=" << stats.pointerMoves
              << " buttons=" << stats.pointerButtons
              << " wheels=" << stats.wheelEvents
              << " keys=" << stats.keyEvents
              << " touches=" << stats.touchEvents
              << " sequence_gaps=" << stats.sequenceGaps
              << " estimated_lost=" << stats.estimatedLostEvents
              << " out_of_order=" << stats.outOfOrderEvents << '\n';
    if (stats.timingEvents > 0) {
        std::cout << "Input timing stats: samples=" << stats.timingEvents
                  << " min_us=" << stats.minLatencyUs
                  << " avg_us=" << stats.averageLatencyUs
                  << " max_us=" << stats.maxLatencyUs
                  << " jitter_us=" << stats.jitterUs
                  << " clock_skewed=" << (stats.minLatencyUs < 0 || stats.maxLatencyUs < 0 ? "yes" : "no")
                  << '\n';
    }
}

led::Status acceptReadyClient(
    std::uint16_t controlPort,
    std::uint16_t rtpPort,
    const led::protocol::VideoMode& videoMode,
    led::transport::TcpStream& stream,
    led::protocol::ClientHello& hello,
    led::protocol::ClientReady& ready,
    int acceptTimeoutMs = 0) {
    led::protocol::SessionOffer offer;
    offer.videoMode = videoMode;
    offer.rtpPort = rtpPort;
    offer.sessionToken = "local-session-token";

    led::transport::TcpListener listener;
    auto status = listener.bindAndListen(controlPort);
    if (!status.isOk()) {
        return status;
    }
    if (acceptTimeoutMs > 0) {
        status = listener.setAcceptTimeoutMs(acceptTimeoutMs);
        if (!status.isOk()) {
            return status;
        }
    }

    std::cout << "Host test signaling listening on TCP " << listener.localPort()
              << ", offering RTP " << offer.rtpPort << '\n';

    status = listener.accept(stream);
    listener.close();
    if (!status.isOk()) {
        return status;
    }

    std::string line;
    status = stream.readLine(line);
    if (!status.isOk()) {
        return status;
    }

    status = led::protocol::parseClientHello(line, hello);
    if (!status.isOk()) {
        return status;
    }

    status = stream.sendLine(led::protocol::serializeSessionOffer(offer));
    if (!status.isOk()) {
        return status;
    }

    status = stream.readLine(line);
    if (!status.isOk()) {
        return status;
    }

    status = led::protocol::parseClientReady(line, ready);
    if (!status.isOk()) {
        return status;
    }
    if (ready.sessionToken != offer.sessionToken) {
        return led::Status::invalidArgument("CLIENT_READY token mismatch");
    }

    return led::Status::ok();
}

int runListenMode(int argc, char** argv) {
    const auto controlPort = argc >= 3 ? parsePort(argv[2], 17660) : std::uint16_t{17660};
    const auto rtpPort = argc >= 4 ? parsePort(argv[3], 17670) : std::uint16_t{17670};

    led::protocol::SessionOffer offer;
    offer.videoMode = led::protocol::defaultStandardMode();
    offer.rtpPort = rtpPort;
    offer.sessionToken = "local-session-token";

    led::host::SignalingServer server;
    auto status = server.listen(controlPort);
    if (!status.isOk()) {
        led::logError(status.message());
        return 1;
    }

    std::cout << "Host signaling listening on TCP " << server.port()
              << ", offering RTP " << offer.rtpPort << '\n';
    std::cout << "Waiting for one client hello...\n";

    led::protocol::ClientHello hello;
    status = server.acceptClientAndSendOffer(offer, hello);
    server.close();
    if (!status.isOk()) {
        led::logError(status.message());
        return 1;
    }

    std::cout << "Client accepted: " << hello.device.name
              << " (" << hello.device.platform << ")\n";
    std::cout << "Session offer sent: "
              << offer.videoMode.resolution.width << 'x'
              << offer.videoMode.resolution.height << '@'
              << offer.videoMode.resolution.refreshRate
              << " codec=" << led::protocol::toString(offer.videoMode.codec)
              << " rtp=" << offer.rtpPort << '\n';
    return 0;
}

int runListDisplaysMode() {
    const auto bounds = led::host::DisplayManager::currentDesktopBounds();
    std::cout << "Virtual desktop: origin=" << bounds.left << ',' << bounds.top
              << " size=" << bounds.width << 'x' << bounds.height << '\n';

    const auto displays = led::host::DisplayManager::enumerateDisplays();
    if (displays.empty()) {
        std::cout << "No active displays found.\n";
        return 0;
    }

    for (const auto& display : displays) {
        std::cout << "Display: " << display.deviceName
                  << " origin=" << display.originX << ',' << display.originY
                  << " size=" << display.width << 'x' << display.height
                  << " primary=" << (display.primary ? "yes" : "no")
                  << " led_virtual=" << (display.ledVirtual ? "yes" : "no") << '\n';
    }
    return 0;
}

int runSendTestNalMode(int argc, char** argv) {
    const auto controlPort = argc >= 3 ? parsePort(argv[2], 17660) : std::uint16_t{17660};
    const auto rtpPort = argc >= 4 ? parsePort(argv[3], 17670) : std::uint16_t{17670};

    led::transport::TcpStream stream;
    led::protocol::ClientHello hello;
    led::protocol::ClientReady ready;
    auto status = acceptReadyClient(controlPort, rtpPort, led::protocol::defaultStandardMode(), stream, hello, ready);
    if (!status.isOk()) {
        led::logError(status.message());
        return 1;
    }

    std::vector<std::uint8_t> testNal(3000, 0x33);
    testNal.front() = 0x65;

    const auto target = led::transport::UdpEndpoint{stream.peerEndpoint().address, ready.rtpPort};
    led::host::VideoSender sender;
    sender.setEmbedSendTime(true);
    status = sender.open(target);
    if (!status.isOk()) {
        led::logError(status.message());
        return 1;
    }
    status = sender.sendNal(testNal, 90000);
    sender.close();
    if (!status.isOk()) {
        led::logError(status.message());
        return 1;
    }

    std::cout << "Client ready: " << hello.device.name
              << " peer=" << target.address << ':' << target.port << '\n';
    std::cout << "Sent test H.264 NAL bytes=" << testNal.size() << '\n';
    return 0;
}

int runSendTestStreamMode(int argc, char** argv) {
    const auto controlPort = argc >= 3 ? parsePort(argv[2], 17660) : std::uint16_t{17660};
    const auto rtpPort = argc >= 4 ? parsePort(argv[3], 17670) : std::uint16_t{17670};
    const auto frameCount = argc >= 5 ? parseU32(argv[4], 120) : std::uint32_t{120};
    const auto fps = argc >= 6 ? parseU32(argv[5], 60) : std::uint32_t{60};

    led::transport::TcpStream stream;
    led::protocol::ClientHello hello;
    led::protocol::ClientReady ready;
    auto status = acceptReadyClient(controlPort, rtpPort, led::protocol::defaultStandardMode(), stream, hello, ready);
    if (!status.isOk()) {
        led::logError(status.message());
        return 1;
    }

    const auto target = led::transport::UdpEndpoint{stream.peerEndpoint().address, ready.rtpPort};
    led::host::VideoSender sender;
    sender.setEmbedSendTime(true);
    status = sender.open(target);
    if (!status.isOk()) {
        led::logError(status.message());
        return 1;
    }

    const auto frameInterval = std::chrono::microseconds(1000000 / (fps == 0 ? 60 : fps));
    const auto timestampStep = 90000 / (fps == 0 ? 60 : fps);
    auto nextFrameTime = std::chrono::steady_clock::now();

    for (std::uint32_t frame = 0; frame < frameCount; ++frame) {
        std::vector<std::uint8_t> nal(3000, static_cast<std::uint8_t>(frame & 0xFF));
        nal.front() = 0x65;
        status = sender.sendNal(nal, 90000 + frame * timestampStep);
        if (!status.isOk()) {
            sender.close();
            led::logError(status.message());
            return 1;
        }

        nextFrameTime += frameInterval;
        std::this_thread::sleep_until(nextFrameTime);
    }

    sender.close();
    std::cout << "Client ready: " << hello.device.name
              << " peer=" << target.address << ':' << target.port << '\n';
    std::cout << "Sent test stream frames=" << frameCount
              << " fps=" << (fps == 0 ? 60 : fps)
              << " nal_bytes=3000\n";
    return 0;
}

int runSendCaptureTestStreamMode(int argc, char** argv) {
    const auto controlPort = argc >= 3 ? parsePort(argv[2], 17660) : std::uint16_t{17660};
    const auto rtpPort = argc >= 4 ? parsePort(argv[3], 17670) : std::uint16_t{17670};
    const auto frameCount = argc >= 5 ? parseU32(argv[4], 120) : std::uint32_t{120};
    const auto fps = argc >= 6 ? parseU32(argv[5], 60) : std::uint32_t{60};
    const auto width = argc >= 7 ? parseU32(argv[6], 1920) : std::uint32_t{1920};
    const auto height = argc >= 8 ? parseU32(argv[7], 1080) : std::uint32_t{1080};
    const auto bitrateKbps = argc >= 9 ? parseU32(argv[8], 20000) : std::uint32_t{20000};
    const auto actualFps = fps == 0 ? std::uint32_t{60} : fps;
    const auto mode = makeVideoMode(width, height, actualFps, bitrateKbps);

    led::transport::TcpStream stream;
    led::protocol::ClientHello hello;
    led::protocol::ClientReady ready;
    auto status = acceptReadyClient(controlPort, rtpPort, mode, stream, hello, ready);
    if (!status.isOk()) {
        led::logError(status.message());
        return 1;
    }

    led::host::CaptureEngine capture;
    status = capture.start(mode.resolution);
    if (!status.isOk()) {
        led::logError(status.message());
        return 1;
    }

    led::host::Encoder encoder;
    status = encoder.configure(mode);
    if (!status.isOk()) {
        capture.stop();
        led::logError(status.message());
        return 1;
    }
    status = encoder.start();
    if (!status.isOk()) {
        capture.stop();
        led::logError(status.message());
        return 1;
    }

    const auto target = led::transport::UdpEndpoint{stream.peerEndpoint().address, ready.rtpPort};
    led::host::VideoSender sender;
    sender.setEmbedSendTime(true);
    status = sender.open(target);
    if (!status.isOk()) {
        encoder.stop();
        capture.stop();
        led::logError(status.message());
        return 1;
    }

    const auto frameInterval = std::chrono::microseconds(1000000 / actualFps);
    auto nextFrameTime = std::chrono::steady_clock::now();
    std::uint64_t bytesSent = 0;
    std::uint64_t nalUnitsSent = 0;
    std::uint64_t keyFrames = 0;
    const std::string encoderBackend = encoder.backendName();

    for (std::uint32_t index = 0; index < frameCount; ++index) {
        led::host::CapturedFrame frame;
        status = capture.captureNextFrame(frame);
        if (!status.isOk()) {
            sender.close();
            encoder.stop();
            capture.stop();
            led::logError(status.message());
            return 1;
        }

        const auto encoded = encoder.encode(frame);
        if (encoded.keyFrame) {
            ++keyFrames;
        }
        bytesSent += encoded.payload.size();
        nalUnitsSent += encoded.nalUnits.size();
        status = sender.sendFrame(encoded);
        if (!status.isOk()) {
            sender.close();
            encoder.stop();
            capture.stop();
            led::logError(status.message());
            return 1;
        }

        nextFrameTime += frameInterval;
        std::this_thread::sleep_until(nextFrameTime);
    }

    sender.close();
    encoder.stop();
    capture.stop();

    std::cout << "Client ready: " << hello.device.name
              << " peer=" << target.address << ':' << target.port << '\n';
    std::cout << "Sent capture test stream frames=" << frameCount
              << " fps=" << actualFps
              << " mode=" << mode.resolution.width << 'x' << mode.resolution.height
              << " bitrate_kbps=" << mode.bitrateKbps
              << " backend=" << encoderBackend
              << " nals=" << nalUnitsSent
              << " keyframes=" << keyFrames
              << " encoded_bytes=" << bytesSent << '\n';
    return 0;
}

int runServeLiveCaptureMode(int argc, char** argv) {
    LiveCaptureStopEvent stopEvent;
    const auto controlPort = argc >= 3 ? parsePort(argv[2], 17660) : std::uint16_t{17660};
    const auto rtpPort = argc >= 4 ? parsePort(argv[3], 17670) : std::uint16_t{17670};
    const auto inputPort = argc >= 5 ? parsePort(argv[4], 17691) : std::uint16_t{17691};
    const auto frameCount = argc >= 6 ? parseU32(argv[5], 0) : std::uint32_t{0};
    const auto fps = argc >= 7 ? parseU32(argv[6], 30) : std::uint32_t{30};
    led::host::InputInjectionBackend backend = led::host::InputInjectionBackend::dryRun;
    if (argc >= 8 && !led::host::parseInputInjectionBackend(argv[7], backend)) {
        std::cerr << "Usage: led_host_app --serve-live-capture [control-port] [rtp-port] [input-port] [frames] [fps] [dry-run|sendinput] [bitrate-kbps] [width] [height]\n";
        return 1;
    }
    const auto bitrateKbps = argc >= 9 ? parseU32(argv[8], 20000) : std::uint32_t{20000};
    const auto width = argc >= 10 ? parseU32(argv[9], 1920) : std::uint32_t{1920};
    const auto height = argc >= 11 ? parseU32(argv[10], 1080) : std::uint32_t{1080};
    const auto actualFps = fps == 0 ? std::uint32_t{30} : fps;
    const auto mode = makeVideoMode(width, height, actualFps, bitrateKbps);

    led::host::DisplayManager displayManager;
    signalDriverCreateMonitor();
    auto status = displayManager.createVirtualDisplay(mode.resolution);
    if (!status.isOk()) {
        led::logError(status.message());
        return 1;
    }

    led::host::InputReceiver inputReceiver;
    status = inputReceiver.bind(inputPort);
    if (!status.isOk()) {
        displayManager.destroyVirtualDisplay();
        displayManager.restoreDisplayLayout();
        led::logError(status.message());
        return 1;
    }
    inputReceiver.setReceiveTimeoutMs(200);
    led::host::InputInjector injector(displayManager, backend);
    std::atomic_bool stopInput{false};
    led::transport::UdpEndpoint lastInputSource;
    led::Status inputStatus;
    std::thread inputThread([&]() {
        auto lastStatsLog = std::chrono::steady_clock::now();
        while (!stopInput.load() && !stopEvent.requested()) {
            led::protocol::InputEvent event;
            led::transport::UdpEndpoint source;
            auto receiveStatus = inputReceiver.receive(event, source);
            const auto now = std::chrono::steady_clock::now();
            if (now - lastStatsLog >= std::chrono::seconds(1)) {
                const auto stats = inputReceiver.stats();
                if (stats.eventsReceived > 0) {
                    std::cout << "Host input live stats: events=" << stats.eventsReceived
                              << " moves=" << stats.pointerMoves
                              << " buttons=" << stats.pointerButtons
                              << " keys=" << stats.keyEvents
                              << " avg_ms=" << (stats.averageLatencyUs / 1000.0)
                              << " jitter_ms=" << (stats.jitterUs / 1000.0)
                              << " sequence_gaps=" << stats.sequenceGaps << '\n';
                }
                lastStatsLog = now;
            }
            if (!receiveStatus.isOk()) {
                continue;
            }
            lastInputSource = source;
            auto injectStatus = injector.inject(event);
            if (!injectStatus.isOk()) {
                inputStatus = injectStatus;
                stopInput = true;
                return;
            }
        }
    });

    led::transport::TcpStream stream;
    led::protocol::ClientHello hello;
    led::protocol::ClientReady ready;
    status = acceptReadyClient(controlPort, rtpPort, mode, stream, hello, ready, 10000);
    if (!status.isOk()) {
        stopInput = true;
        if (inputThread.joinable()) {
            inputThread.join();
        }
        inputReceiver.close();
        displayManager.destroyVirtualDisplay();
        displayManager.restoreDisplayLayout();
        led::logError(status.message());
        return 1;
    }

    led::host::CaptureEngine capture;
    const auto& display = displayManager.virtualDisplay();
    status = capture.startRegion(mode.resolution, display.originX, display.originY, display.deviceName);
    if (!status.isOk()) {
        stopInput = true;
        if (inputThread.joinable()) {
            inputThread.join();
        }
        inputReceiver.close();
        displayManager.destroyVirtualDisplay();
        displayManager.restoreDisplayLayout();
        led::logError(status.message());
        return 1;
    }

    led::host::Encoder encoder;
    status = encoder.configure(mode);
    if (!status.isOk()) {
        capture.stop();
        stopInput = true;
        if (inputThread.joinable()) {
            inputThread.join();
        }
        inputReceiver.close();
        displayManager.destroyVirtualDisplay();
        displayManager.restoreDisplayLayout();
        led::logError(status.message());
        return 1;
    }
    status = encoder.start();
    if (!status.isOk()) {
        capture.stop();
        stopInput = true;
        if (inputThread.joinable()) {
            inputThread.join();
        }
        inputReceiver.close();
        displayManager.destroyVirtualDisplay();
        displayManager.restoreDisplayLayout();
        led::logError(status.message());
        return 1;
    }

    const auto target = led::transport::UdpEndpoint{stream.peerEndpoint().address, ready.rtpPort};
    led::host::VideoSender sender;
    sender.setEmbedSendTime(true);
    status = sender.open(target);
    if (!status.isOk()) {
        encoder.stop();
        capture.stop();
        stopInput = true;
        if (inputThread.joinable()) {
            inputThread.join();
        }
        inputReceiver.close();
        displayManager.destroyVirtualDisplay();
        displayManager.restoreDisplayLayout();
        led::logError(status.message());
        return 1;
    }

    FrameAckTracker frameAckTracker;
    auto frameAckThread = startFrameAckListener(frameAckTracker, stopInput);
    const auto frameInterval = std::chrono::microseconds(1000000 / actualFps);
    auto nextFrameTime = std::chrono::steady_clock::now();
    std::uint32_t capturedFrames = 0;
    std::uint64_t bytesSent = 0;
    std::uint64_t nalUnitsSent = 0;
    std::uint64_t keyFrames = 0;
    const std::string encoderBackend = encoder.backendName();
    auto lastVideoStatsLog = std::chrono::steady_clock::now();
    auto lastCaptureRebindAttempt = std::chrono::steady_clock::now();
    auto recoverCapture = [&]() -> led::Status {
        for (int attempt = 0; attempt < 80 && !stopInput.load() && !stopEvent.requested(); ++attempt) {
            signalDriverCreateMonitor();
            auto displayStatus = displayManager.createVirtualDisplay(mode.resolution);
            if (displayStatus.isOk()) {
                const auto& recoveredDisplay = displayManager.virtualDisplay();
                auto restartStatus = capture.startRegion(
                    mode.resolution,
                    recoveredDisplay.originX,
                    recoveredDisplay.originY,
                    recoveredDisplay.deviceName);
                if (restartStatus.isOk()) {
                    led::logInfo(
                        "host capture rebound to LED virtual display " + recoveredDisplay.deviceName +
                        " at " + std::to_string(recoveredDisplay.originX) + "," +
                        std::to_string(recoveredDisplay.originY));
                    return led::Status::ok();
                }
                capture.stop();
                displayStatus = restartStatus;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            status = displayStatus;
        }
        return status.isOk() ? led::Status::unavailable("capture recovery timed out") : status;
    };

    while (!stopInput.load() && !stopEvent.requested() && (frameCount == 0 || capturedFrames < frameCount)) {
        led::host::CapturedFrame frame;
        status = capture.captureNextFrame(frame);
        if (!status.isOk()) {
            led::logWarn("capture failed, rebinding virtual display: " + status.message());
            capture.stop();
            status = recoverCapture();
            if (!status.isOk()) {
                break;
            }
            continue;
        }

        const auto encoded = encoder.encode(frame);
        if (encoded.keyFrame) {
            ++keyFrames;
        }
        const bool hasEncodedData = !encoded.payload.empty() || !encoded.nalUnits.empty();
        bytesSent += encoded.payload.size();
        nalUnitsSent += encoded.nalUnits.size();
        if (hasEncodedData) {
            noteFrameSent(frameAckTracker, encoded.frameId);
        }
        status = sender.sendFrame(encoded);
        if (!status.isOk()) {
            break;
        }

        ++capturedFrames;
        const auto now = std::chrono::steady_clock::now();
        if (!capture.dxgiCapture_ && now - lastCaptureRebindAttempt >= std::chrono::seconds(2)) {
            lastCaptureRebindAttempt = now;
            led::logWarn("capture is running on fallback backend; trying to restore LED virtual display binding");
            capture.stop();
            const auto recoverStatus = recoverCapture();
            if (!recoverStatus.isOk()) {
                led::logWarn("LED virtual display restore attempt failed: " + recoverStatus.message());
                auto fallbackStatus = capture.startRegion(
                    mode.resolution,
                    displayManager.virtualDisplay().originX,
                    displayManager.virtualDisplay().originY,
                    displayManager.virtualDisplay().deviceName);
                if (!fallbackStatus.isOk()) {
                    status = fallbackStatus;
                    break;
                }
            }
        }
        if (now - lastVideoStatsLog >= std::chrono::seconds(1)) {
            const auto [receiveAckStats, renderAckStats] = snapshotFrameAckStats(frameAckTracker);
            std::cout << "Host video live stats: frames=" << capturedFrames
                      << " nals=" << nalUnitsSent
                      << " keyframes=" << keyFrames
                      << " encoded_kb=" << (bytesSent / 1024)
                      << " fps_target=" << actualFps
                      << " receive_ack_count=" << receiveAckStats.acks
                      << " receive_ack_avg_ms=" << (receiveAckStats.averageRttUs / 1000.0)
                      << " receive_ack_max_ms=" << (receiveAckStats.maxRttUs / 1000.0)
                      << " render_ack_count=" << renderAckStats.acks
                      << " render_ack_avg_ms=" << (renderAckStats.averageRttUs / 1000.0)
                      << " render_ack_max_ms=" << (renderAckStats.maxRttUs / 1000.0)
                      << " render_ack_last_frame=" << renderAckStats.lastFrameId
                      << " ack_unknown=" << (receiveAckStats.unknownAcks + renderAckStats.unknownAcks) << '\n';
            lastVideoStatsLog = now;
        }
        nextFrameTime += frameInterval;
        std::this_thread::sleep_until(nextFrameTime);
    }

    sender.close();
    encoder.stop();
    capture.stop();
    stopInput = true;
    if (frameAckThread.joinable()) {
        frameAckThread.join();
    }
    if (inputThread.joinable()) {
        inputThread.join();
    }
    const auto inputStats = inputReceiver.stats();
    inputReceiver.close();
    displayManager.destroyVirtualDisplay();
    displayManager.restoreDisplayLayout();

    if (!status.isOk()) {
        led::logError(status.message());
        return 1;
    }
    if (!inputStatus.isOk()) {
        led::logError(inputStatus.message());
        return 1;
    }

    std::cout << "Client ready: " << hello.device.name
              << " peer=" << target.address << ':' << target.port
              << " input_port=" << inputPort
              << " input_backend=" << injector.backendName() << '\n';
    std::cout << "Served live capture frames=" << capturedFrames
              << " fps=" << actualFps
              << " mode=" << mode.resolution.width << 'x' << mode.resolution.height
              << " bitrate_kbps=" << mode.bitrateKbps
              << " backend=" << encoderBackend
              << " nals=" << nalUnitsSent
              << " keyframes=" << keyFrames
              << " encoded_bytes=" << bytesSent << '\n';
    if (inputStats.eventsReceived > 0) {
        std::cout << "Last input source: " << lastInputSource.address << ':' << lastInputSource.port << '\n';
    }
    printInputReceiverStats(inputStats);
    return 0;
}

int runServeMjpegCaptureMode(int argc, char** argv) {
    LiveCaptureStopEvent stopEvent;
    const auto controlPort = argc >= 3 ? parsePort(argv[2], 17660) : std::uint16_t{17660};
    const auto udpPort = argc >= 4 ? parsePort(argv[3], 17670) : std::uint16_t{17670};
    const auto frameCount = argc >= 5 ? parseU32(argv[4], 0) : std::uint32_t{0};
    const auto fps = argc >= 6 ? parseU32(argv[5], 60) : std::uint32_t{60};
    const auto qualityPercent = argc >= 7 ? parseU32(argv[6], 55) : std::uint32_t{55};
    const auto width = argc >= 8 ? parseU32(argv[7], 1920) : std::uint32_t{1920};
    const auto height = argc >= 9 ? parseU32(argv[8], 1080) : std::uint32_t{1080};
    const auto inputPort = argc >= 10 ? parsePort(argv[9], 17691) : std::uint16_t{17691};
    led::host::InputInjectionBackend backend = led::host::InputInjectionBackend::sendInput;
    if (argc >= 11 && !led::host::parseInputInjectionBackend(argv[10], backend)) {
        std::cerr << "Usage: led_host_app --serve-mjpeg-capture [control-port] [udp-port] [frames] [fps] [quality-percent] [width] [height] [input-port] [dry-run|sendinput]\n";
        return 1;
    }
    const auto actualFps = fps == 0 ? std::uint32_t{60} : fps;
    const auto mode = makeVideoMode(width, height, actualFps, 0);

    led::host::DisplayManager displayManager;
    signalDriverCreateMonitor();
    auto status = displayManager.createVirtualDisplay(mode.resolution);
    if (!status.isOk()) {
        led::logError(status.message());
        return 1;
    }

    led::host::InputReceiver inputReceiver;
    status = inputReceiver.bind(inputPort);
    if (!status.isOk()) {
        displayManager.destroyVirtualDisplay();
        displayManager.restoreDisplayLayout();
        led::logError(status.message());
        return 1;
    }
    inputReceiver.setReceiveTimeoutMs(200);
    led::host::InputInjector injector(displayManager, backend);
    std::atomic_bool stopInput{false};
    led::Status inputStatus;
    std::thread inputThread([&]() {
        auto lastStatsLog = std::chrono::steady_clock::now();
        while (!stopInput.load() && !stopEvent.requested()) {
            led::protocol::InputEvent event;
            led::transport::UdpEndpoint source;
            auto receiveStatus = inputReceiver.receive(event, source);
            const auto now = std::chrono::steady_clock::now();
            if (now - lastStatsLog >= std::chrono::seconds(1)) {
                const auto stats = inputReceiver.stats();
                if (stats.eventsReceived > 0) {
                    std::cout << "Host input live stats: events=" << stats.eventsReceived
                              << " moves=" << stats.pointerMoves
                              << " buttons=" << stats.pointerButtons
                              << " keys=" << stats.keyEvents
                              << " avg_ms=" << (stats.averageLatencyUs / 1000.0)
                              << " jitter_ms=" << (stats.jitterUs / 1000.0)
                              << " sequence_gaps=" << stats.sequenceGaps << '\n';
                }
                lastStatsLog = now;
            }
            if (!receiveStatus.isOk()) {
                continue;
            }
            auto injectStatus = injector.inject(event);
            if (!injectStatus.isOk()) {
                inputStatus = injectStatus;
                stopInput = true;
                return;
            }
        }
    });
    auto stopInputThread = [&]() {
        stopInput = true;
        if (inputThread.joinable()) {
            inputThread.join();
        }
        inputReceiver.close();
    };

    led::transport::TcpStream stream;
    led::protocol::ClientHello hello;
    led::protocol::ClientReady ready;
      status = acceptReadyClient(controlPort, udpPort, mode, stream, hello, ready, 10000);
      if (!status.isOk()) {
          stopInputThread();
          displayManager.destroyVirtualDisplay();
          displayManager.restoreDisplayLayout();
          led::logError(status.message());
        return 1;
    }

    led::host::CaptureEngine capture;
    const auto& display = displayManager.virtualDisplay();
      status = capture.startRegion(mode.resolution, display.originX, display.originY, display.deviceName);
      if (!status.isOk()) {
          stopInputThread();
          displayManager.destroyVirtualDisplay();
          displayManager.restoreDisplayLayout();
          led::logError(status.message());
        return 1;
    }

    led::transport::UdpSocket socket;
      status = socket.open();
      if (!status.isOk()) {
          capture.stop();
          stopInputThread();
          displayManager.destroyVirtualDisplay();
          displayManager.restoreDisplayLayout();
          led::logError(status.message());
        return 1;
    }
    const auto target = led::transport::UdpEndpoint{stream.peerEndpoint().address, ready.rtpPort};

    std::atomic_bool stopAck{false};
    FrameAckTracker frameAckTracker;
    auto frameAckThread = startFrameAckListener(frameAckTracker, stopAck);
    const auto frameInterval = std::chrono::microseconds(1000000 / actualFps);
    auto nextFrameTime = std::chrono::steady_clock::now();
    auto lastVideoStatsLog = nextFrameTime;
    std::uint32_t capturedFrames = 0;
    std::uint64_t encodedBytes = 0;
    std::uint64_t packetsSent = 0;
    std::uint64_t dirtyFrames = 0;
    std::uint64_t fullFrames = 0;
    std::uint64_t skippedFrames = 0;
    std::uint64_t dirtyAreaPixels = 0;
    const float quality = std::clamp(static_cast<float>(qualityPercent) / 100.0F, 0.1F, 1.0F);
    led::host::CapturedFrame previousFrame;
    const auto keyFrameInterval = std::max<std::uint32_t>(1, actualFps);

    while (!stopEvent.requested() && (frameCount == 0 || capturedFrames < frameCount)) {
        led::host::CapturedFrame frame;
        status = capture.captureNextFrame(frame);
        if (!status.isOk()) {
            break;
        }

        const bool forceKeyFrame = previousFrame.bgra.empty() || (capturedFrames % keyFrameInterval) == 0;
        const auto dirty = forceKeyFrame
            ? fullDirtyRect(frame.width, frame.height)
            : computeDirtyRect(frame, previousFrame, 16, 0.70);
        previousFrame = frame;
        if (dirty.empty) {
            ++capturedFrames;
            ++skippedFrames;
            nextFrameTime += frameInterval;
            std::this_thread::sleep_until(nextFrameTime);
            continue;
        }

        led::host::EncodedJpegFrame jpeg;
        status = led::host::encodeBgraJpegRect(frame, dirty.x, dirty.y, dirty.width, dirty.height, quality, jpeg);
        if (!status.isOk()) {
            break;
        }
        const auto packets = led::transport::packetizeMjpegFrameRect(
            jpeg.jpegBytes,
            jpeg.frameId,
            jpeg.timestampUs,
            dirty.fullFrame,
            frame.width,
            frame.height,
            dirty.x,
            dirty.y,
            dirty.width,
            dirty.height,
            1200);
        if (packets.empty()) {
            status = led::Status::unavailable("MJPEG packetization produced no packets");
            break;
        }
        noteFrameSent(frameAckTracker, jpeg.frameId);
        for (const auto& packet : packets) {
            status = socket.sendTo(packet, target);
            if (!status.isOk()) {
                break;
            }
            ++packetsSent;
        }
        if (!status.isOk()) {
            break;
        }

        ++capturedFrames;
        encodedBytes += jpeg.jpegBytes.size();
        dirtyAreaPixels += static_cast<std::uint64_t>(dirty.width) * dirty.height;
        if (dirty.fullFrame) {
            ++fullFrames;
        } else {
            ++dirtyFrames;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - lastVideoStatsLog >= std::chrono::seconds(1)) {
            const auto [receiveAckStats, renderAckStats] = snapshotFrameAckStats(frameAckTracker);
            std::cout << "Host MJPEG live stats: frames=" << capturedFrames
                      << " packets=" << packetsSent
                      << " encoded_kb=" << (encodedBytes / 1024)
                      << " avg_frame_kb=" << (capturedFrames == 0 ? 0 : (encodedBytes / 1024 / capturedFrames))
                      << " full=" << fullFrames
                      << " dirty=" << dirtyFrames
                      << " skipped=" << skippedFrames
                      << " avg_dirty_pct=" << (capturedFrames == 0 ? 0.0 :
                          (static_cast<double>(dirtyAreaPixels) * 100.0 /
                           (static_cast<double>(capturedFrames) * mode.resolution.width * mode.resolution.height)))
                      << " fps_target=" << actualFps
                      << " receive_ack_count=" << receiveAckStats.acks
                      << " receive_ack_avg_ms=" << (receiveAckStats.averageRttUs / 1000.0)
                      << " receive_ack_max_ms=" << (receiveAckStats.maxRttUs / 1000.0)
                      << " render_ack_count=" << renderAckStats.acks
                      << " render_ack_avg_ms=" << (renderAckStats.averageRttUs / 1000.0)
                      << " render_ack_max_ms=" << (renderAckStats.maxRttUs / 1000.0)
                      << " render_ack_last_frame=" << renderAckStats.lastFrameId
                      << " ack_unknown=" << (receiveAckStats.unknownAcks + renderAckStats.unknownAcks) << '\n';
            lastVideoStatsLog = now;
        }
        nextFrameTime += frameInterval;
        std::this_thread::sleep_until(nextFrameTime);
    }

    socket.close();
      capture.stop();
      stopAck = true;
      if (frameAckThread.joinable()) {
          frameAckThread.join();
      }
      stopInputThread();
      displayManager.destroyVirtualDisplay();
      displayManager.restoreDisplayLayout();
      if (status.isOk() && !inputStatus.isOk()) {
          status = inputStatus;
      }
    if (!status.isOk()) {
        led::logError(status.message());
        return 1;
    }

    std::cout << "Client ready: " << hello.device.name
              << " peer=" << target.address << ':' << target.port << '\n';
    std::cout << "Served MJPEG capture frames=" << capturedFrames
              << " fps=" << actualFps
              << " mode=" << mode.resolution.width << 'x' << mode.resolution.height
              << " quality=" << quality
              << " full=" << fullFrames
              << " dirty=" << dirtyFrames
              << " skipped=" << skippedFrames
              << " packets=" << packetsSent
              << " encoded_bytes=" << encodedBytes << '\n';
    return 0;
}

int runSendH264FileMode(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: led_host_app --send-h264-file <file.h264> [control-port] [rtp-port] [fps] [loops]\n";
        return 1;
    }

    const char* path = argv[2];
    const auto controlPort = argc >= 4 ? parsePort(argv[3], 17660) : std::uint16_t{17660};
    const auto rtpPort = argc >= 5 ? parsePort(argv[4], 17670) : std::uint16_t{17670};
    const auto fps = argc >= 6 ? parseU32(argv[5], 60) : std::uint32_t{60};
    const auto loops = argc >= 7 ? parseU32(argv[6], 1) : std::uint32_t{1};

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

    led::transport::TcpStream stream;
    led::protocol::ClientHello hello;
    led::protocol::ClientReady ready;
    status = acceptReadyClient(controlPort, rtpPort, led::protocol::defaultStandardMode(), stream, hello, ready);
    if (!status.isOk()) {
        led::logError(status.message());
        return 1;
    }

    const auto target = led::transport::UdpEndpoint{stream.peerEndpoint().address, ready.rtpPort};
    led::host::VideoSender sender;
    sender.setEmbedSendTime(true);
    status = sender.open(target);
    if (!status.isOk()) {
        led::logError(status.message());
        return 1;
    }

    const auto frameInterval = std::chrono::microseconds(1000000 / (fps == 0 ? 60 : fps));
    const auto timestampStep = 90000 / (fps == 0 ? 60 : fps);
    auto nextFrameTime = std::chrono::steady_clock::now();
    std::uint32_t frameIndex = 0;
    std::uint64_t bytesSent = 0;

    const auto loopCount = loops == 0 ? std::uint32_t{1} : loops;
    for (std::uint32_t loop = 0; loop < loopCount; ++loop) {
        for (const auto& nal : nalUnits) {
            const auto type = led::transport::h264NalType(nal.bytes);
            const bool parameterSet = type == 7 || type == 8 || type == 9;
            const auto timestamp = 90000 + frameIndex * timestampStep;
            status = sender.sendNal(nal.bytes, timestamp);
            if (!status.isOk()) {
                sender.close();
                led::logError(status.message());
                return 1;
            }
            bytesSent += nal.bytes.size();

            if (!parameterSet) {
                ++frameIndex;
                nextFrameTime += frameInterval;
                std::this_thread::sleep_until(nextFrameTime);
            }
        }
    }

    sender.close();
    std::cout << "Client ready: " << hello.device.name
              << " peer=" << target.address << ':' << target.port << '\n';
    std::cout << "Sent Annex B file NAL units=" << nalUnits.size()
              << " loops=" << loopCount
              << " frame_like_nals=" << frameIndex
              << " bytes=" << bytesSent
              << " fps=" << (fps == 0 ? 60 : fps) << '\n';
    return 0;
}

int runServeH264FileWithInputMode(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: led_host_app --serve-h264-file <file.h264> [control-port] [rtp-port] [input-port] [fps] [loops] [dry-run|sendinput]\n";
        return 1;
    }

    const char* path = argv[2];
    const auto controlPort = argc >= 4 ? parsePort(argv[3], 17660) : std::uint16_t{17660};
    const auto rtpPort = argc >= 5 ? parsePort(argv[4], 17670) : std::uint16_t{17670};
    const auto inputPort = argc >= 6 ? parsePort(argv[5], 17691) : std::uint16_t{17691};
    const auto fps = argc >= 7 ? parseU32(argv[6], 60) : std::uint32_t{60};
    const auto loops = argc >= 8 ? parseU32(argv[7], 1) : std::uint32_t{1};
    led::host::InputInjectionBackend backend = led::host::InputInjectionBackend::dryRun;
    if (argc >= 9 && !led::host::parseInputInjectionBackend(argv[8], backend)) {
        std::cerr << "Usage: led_host_app --serve-h264-file <file.h264> [control-port] [rtp-port] [input-port] [fps] [loops] [dry-run|sendinput]\n";
        return 1;
    }

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

    led::host::SessionManager sessionManager;
    status = sessionManager.start(led::protocol::defaultStandardMode());
    if (!status.isOk()) {
        led::logError(status.message());
        return 1;
    }

    led::host::InputReceiver inputReceiver;
    status = inputReceiver.bind(inputPort);
    if (!status.isOk()) {
        sessionManager.stop();
        led::logError(status.message());
        return 1;
    }
    inputReceiver.setReceiveTimeoutMs(200);
    led::host::InputInjector injector(sessionManager.displayManager(), backend);
    std::atomic_bool stopInput{false};
    led::transport::UdpEndpoint lastInputSource;
    led::Status inputStatus;
    std::thread inputThread([&]() {
        while (!stopInput.load()) {
            led::protocol::InputEvent event;
            led::transport::UdpEndpoint source;
            auto receiveStatus = inputReceiver.receive(event, source);
            if (!receiveStatus.isOk()) {
                continue;
            }
            lastInputSource = source;
            auto injectStatus = injector.inject(event);
            if (!injectStatus.isOk()) {
                inputStatus = injectStatus;
                stopInput = true;
                return;
            }
        }
    });

    led::transport::TcpStream stream;
    led::protocol::ClientHello hello;
    led::protocol::ClientReady ready;
    status = acceptReadyClient(controlPort, rtpPort, led::protocol::defaultStandardMode(), stream, hello, ready);
    if (!status.isOk()) {
        stopInput = true;
        if (inputThread.joinable()) {
            inputThread.join();
        }
        inputReceiver.close();
        sessionManager.stop();
        led::logError(status.message());
        return 1;
    }

    const auto target = led::transport::UdpEndpoint{stream.peerEndpoint().address, ready.rtpPort};
    led::host::VideoSender sender;
    sender.setEmbedSendTime(true);
    status = sender.open(target);
    if (!status.isOk()) {
        stopInput = true;
        if (inputThread.joinable()) {
            inputThread.join();
        }
        inputReceiver.close();
        sessionManager.stop();
        led::logError(status.message());
        return 1;
    }

    const auto actualFps = fps == 0 ? std::uint32_t{60} : fps;
    const auto frameInterval = std::chrono::microseconds(1000000 / actualFps);
    const auto timestampStep = 90000 / actualFps;
    auto nextFrameTime = std::chrono::steady_clock::now();
    std::uint32_t frameIndex = 0;
    std::uint64_t bytesSent = 0;

    const auto loopCount = loops == 0 ? std::uint32_t{1} : loops;
    for (std::uint32_t loop = 0; loop < loopCount; ++loop) {
        for (const auto& nal : nalUnits) {
            const auto type = led::transport::h264NalType(nal.bytes);
            const bool parameterSet = type == 7 || type == 8 || type == 9;
            const auto timestamp = 90000 + frameIndex * timestampStep;
            status = sender.sendNal(nal.bytes, timestamp);
            if (!status.isOk() || stopInput.load()) {
                sender.close();
                stopInput = true;
                if (inputThread.joinable()) {
                    inputThread.join();
                }
                const auto finalStatus = status.isOk() ? inputStatus : status;
                inputReceiver.close();
                sessionManager.stop();
                led::logError(finalStatus.message());
                return 1;
            }
            bytesSent += nal.bytes.size();

            if (!parameterSet) {
                ++frameIndex;
                nextFrameTime += frameInterval;
                std::this_thread::sleep_until(nextFrameTime);
            }
        }
    }

    sender.close();
    stopInput = true;
    if (inputThread.joinable()) {
        inputThread.join();
    }
    const auto inputStats = inputReceiver.stats();
    inputReceiver.close();
    sessionManager.stop();

    std::cout << "Client ready: " << hello.device.name
              << " peer=" << target.address << ':' << target.port
              << " input_port=" << inputPort
              << " input_backend=" << injector.backendName() << '\n';
    std::cout << "Served Annex B file NAL units=" << nalUnits.size()
              << " loops=" << loopCount
              << " frame_like_nals=" << frameIndex
              << " bytes=" << bytesSent
              << " fps=" << actualFps << '\n';
    if (inputStats.eventsReceived > 0) {
        std::cout << "Last input source: " << lastInputSource.address << ':' << lastInputSource.port << '\n';
    }
    printInputReceiverStats(inputStats);
    return 0;
}

int runReceiveTestInputMode(int argc, char** argv) {
    const auto inputPort = argc >= 3 ? parsePort(argv[2], 17691) : std::uint16_t{17691};
    const auto expectedEvents = argc >= 4 ? parseU32(argv[3], 10) : std::uint32_t{10};
    led::host::InputInjectionBackend backend = led::host::InputInjectionBackend::dryRun;
    if (argc >= 5 && !led::host::parseInputInjectionBackend(argv[4], backend)) {
        std::cerr << "Usage: led_host_app --receive-test-input [input-port] [expected-events] [dry-run|sendinput]\n";
        return 1;
    }

    led::host::SessionManager sessionManager;
    auto status = sessionManager.start(led::protocol::defaultStandardMode());
    if (!status.isOk()) {
        led::logError(status.message());
        return 1;
    }

    led::host::InputReceiver receiver;
    status = receiver.bind(inputPort);
    if (!status.isOk()) {
        sessionManager.stop();
        led::logError(status.message());
        return 1;
    }
    receiver.setReceiveTimeoutMs(3000);

    led::host::InputInjector injector(sessionManager.displayManager(), backend);
    std::uint32_t receivedEvents = 0;
    std::uint64_t lastSequence = 0;
    led::transport::UdpEndpoint source;
    const auto start = std::chrono::steady_clock::now();
    while (receivedEvents < expectedEvents) {
        led::protocol::InputEvent event;
        status = receiver.receive(event, source);
        if (!status.isOk()) {
            receiver.close();
            sessionManager.stop();
            led::logError(status.message());
            return 1;
        }

        status = injector.inject(event);
        if (!status.isOk()) {
            receiver.close();
            sessionManager.stop();
            led::logError(status.message());
            return 1;
        }

        ++receivedEvents;
        lastSequence = event.sequence;
    }

    const auto elapsed = std::chrono::steady_clock::now() - start;
    receiver.close();
    sessionManager.stop();

    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    const auto stats = receiver.stats();
    std::cout << "Received input events count=" << receivedEvents
              << " last_seq=" << lastSequence
              << " backend=" << injector.backendName()
              << " elapsed_ms=" << elapsedMs
              << " from " << source.address << ':' << source.port << '\n';
    printInputReceiverStats(stats);
    return 0;
}

int runSkeletonMode() {
    led::logInfo("LAN extended display Windows host skeleton starting");

    led::host::SessionManager sessionManager;
    const auto mode = led::protocol::defaultStandardMode();
    const auto status = sessionManager.start(mode);

    if (!status.isOk()) {
        led::logError(status.message());
        return 1;
    }

    const auto& display = sessionManager.displayManager().virtualDisplay();
    std::cout << "Host skeleton state: " << led::session::toString(sessionManager.state()) << '\n';
    std::cout << "Virtual display: "
              << display.resolution.width << 'x' << display.resolution.height
              << "@" << display.resolution.refreshRate << "Hz\n";
    std::cout << "Codec: " << led::protocol::toString(mode.codec)
              << ", preset: " << led::protocol::toString(mode.preset)
              << ", bitrate: " << mode.bitrateKbps << " kbps\n";

    sessionManager.stop();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    enableProcessDpiAwareness();
    std::cout.setf(std::ios::unitbuf);
    std::cerr.setf(std::ios::unitbuf);

    if (argc >= 2 && std::string(argv[1]) == "--restore-cursor") {
        led::host::CursorSuppressor cursorSuppressor;
        const auto status = cursorSuppressor.hideSystemCursor();
        if (status.isOk()) {
            cursorSuppressor.restoreSystemCursor();
        } else {
            led::logWarn("cursor restore helper could not hide first; forcing restore attempt: " + status.message());
            cursorSuppressor.restoreSystemCursor();
        }
        return 0;
    }
    if (argc >= 2 && std::string(argv[1]) == "--list-displays") {
        return runListDisplaysMode();
    }
    if (argc >= 2 && std::string(argv[1]) == "--listen") {
        return runListenMode(argc, argv);
    }
    if (argc >= 2 && std::string(argv[1]) == "--send-test-nal") {
        return runSendTestNalMode(argc, argv);
    }
    if (argc >= 2 && std::string(argv[1]) == "--send-test-stream") {
        return runSendTestStreamMode(argc, argv);
    }
    if (argc >= 2 && std::string(argv[1]) == "--send-capture-test-stream") {
        return runSendCaptureTestStreamMode(argc, argv);
    }
    if (argc >= 2 && std::string(argv[1]) == "--serve-live-capture") {
        return runServeLiveCaptureMode(argc, argv);
    }
    if (argc >= 2 && std::string(argv[1]) == "--serve-mjpeg-capture") {
        return runServeMjpegCaptureMode(argc, argv);
    }
    if (argc >= 2 && std::string(argv[1]) == "--send-h264-file") {
        return runSendH264FileMode(argc, argv);
    }
    if (argc >= 2 && std::string(argv[1]) == "--serve-h264-file") {
        return runServeH264FileWithInputMode(argc, argv);
    }
    if (argc >= 2 && std::string(argv[1]) == "--receive-test-input") {
        return runReceiveTestInputMode(argc, argv);
    }

    return runSkeletonMode();
}
