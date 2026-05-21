#include "led/client/decoder.h"

#include "led/common/logger.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#if defined(LED_HAS_GSTREAMER)
#include <gst/gst.h>
#endif

namespace led::client {

#if defined(LED_HAS_GSTREAMER)
namespace {

constexpr std::uint64_t kFrameIdPtsUnitNs = 1000000;

RawFrameInfo rawFrameInfoFromSample(GstBuffer* buffer, GstPad* pad) {
    RawFrameInfo info;
    if (buffer != nullptr) {
        info.bytes = gst_buffer_get_size(buffer);
        if (GST_BUFFER_PTS_IS_VALID(buffer)) {
            info.ptsNs = static_cast<std::uint64_t>(GST_BUFFER_PTS(buffer));
        }
    }

    GstCaps* caps = pad != nullptr ? gst_pad_get_current_caps(pad) : nullptr;
    if (caps != nullptr) {
        const auto* structure = gst_caps_get_structure(caps, 0);
        if (structure != nullptr) {
            int width = 0;
            int height = 0;
            if (gst_structure_get_int(structure, "width", &width) && width > 0) {
                info.width = static_cast<std::uint32_t>(width);
            }
            if (gst_structure_get_int(structure, "height", &height) && height > 0) {
                info.height = static_cast<std::uint32_t>(height);
            }
            const auto* format = gst_structure_get_string(structure, "format");
            if (format != nullptr) {
                info.format = format;
            }
        }
        gst_caps_unref(caps);
    }
    if (info.stride == 0 && info.height > 0 && info.bytes > 0) {
        info.stride = static_cast<std::uint32_t>(info.bytes / info.height);
    }
    return info;
}

void onDecodedFrameHandoff(GstElement*, GstBuffer* buffer, GstPad* pad, gpointer userData) {
    auto* decoder = static_cast<Decoder*>(userData);
    if (decoder == nullptr) {
        return;
    }
    const auto frame = rawFrameInfoFromSample(buffer, pad);
    GstMapInfo map{};
    if (buffer != nullptr && gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        decoder->noteDecodedFrameData(frame, map.data);
        gst_buffer_unmap(buffer, &map);
        return;
    }
    decoder->noteDecodedFrame(frame);
}

}  // namespace
#endif

void Decoder::setSinkPipeline(std::string sinkPipeline) {
    if (!sinkPipeline.empty()) {
        sinkPipeline_ = std::move(sinkPipeline);
    }
}

void Decoder::setInputPipeline(std::string sourceCaps, std::string parserPipeline) {
    if (!sourceCaps.empty()) {
        sourceCaps_ = std::move(sourceCaps);
    }
    parserPipeline_ = std::move(parserPipeline);
}

void Decoder::setRenderer(Renderer* renderer) {
    renderer_ = renderer;
}

void Decoder::setRenderedFrameCallback(std::function<void(std::uint64_t)> callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    renderedFrameCallback_ = std::move(callback);
}

Status Decoder::configure(const protocol::VideoMode& mode) {
    if (mode.codec != protocol::Codec::h264) {
        return Status::invalidArgument("MVP decoder only accepts H.264 mode");
    }

    mode_ = mode;
    logInfo(std::string("client decoder configured with backend ") + backendName() + " sink=\"" + sinkPipeline_ + "\"");
    return Status::ok();
}

Status Decoder::start() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stats_ = {};
        pendingFrameIds_.clear();
        lastQueuedFrameId_ = 0;
    }
    auto status = startBackend();
    if (!status.isOk()) {
        return status;
    }
    running_ = true;
    logInfo("client decoder started");
    return Status::ok();
}

Status Decoder::pushNal(const std::vector<std::uint8_t>& nalUnit) {
    if (!running_) {
        return Status::invalidState("cannot push NAL while decoder is stopped");
    }
    if (nalUnit.empty()) {
        return Status::invalidArgument("cannot push empty H.264 NAL");
    }

    const auto nalType = static_cast<unsigned int>(nalUnit.front() & 0x1F);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++stats_.nalUnits;
        stats_.bytes += nalUnit.size();
        if (nalType == 5) {
            ++stats_.keyFrames;
        }
    }

    auto status = pushNalToBackend(nalUnit);
    if (!status.isOk()) {
        return status;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++stats_.backendAccepted;
    }
    return Status::ok();
}

Status Decoder::pushNalWithFrameId(const std::vector<std::uint8_t>& nalUnit, std::uint64_t frameId) {
    rememberFrameId(frameId);
    return pushNal(nalUnit);
}

Status Decoder::pushAccessUnit(const std::vector<std::vector<std::uint8_t>>& nalUnits) {
    if (!running_) {
        return Status::invalidState("cannot push access unit while decoder is stopped");
    }
    if (nalUnits.empty()) {
        return Status::invalidArgument("cannot push empty H.264 access unit");
    }

    std::uint64_t bytes = 0;
    std::uint64_t keyFrames = 0;
    for (const auto& nalUnit : nalUnits) {
        if (nalUnit.empty()) {
            return Status::invalidArgument("cannot push H.264 access unit with an empty NAL");
        }
        bytes += nalUnit.size();
        if ((nalUnit.front() & 0x1F) == 5) {
            ++keyFrames;
        }
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        stats_.nalUnits += nalUnits.size();
        stats_.bytes += bytes;
        stats_.keyFrames += keyFrames;
    }

    auto status = pushAccessUnitToBackend(nalUnits, 0);
    if (!status.isOk()) {
        return status;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stats_.backendAccepted += nalUnits.size();
    }
    return Status::ok();
}

Status Decoder::pushAccessUnitWithFrameId(
    const std::vector<std::vector<std::uint8_t>>& nalUnits,
    std::uint64_t frameId) {
    rememberFrameId(frameId);
    if (!running_) {
        return Status::invalidState("cannot push access unit while decoder is stopped");
    }
    if (nalUnits.empty()) {
        return Status::invalidArgument("cannot push empty H.264 access unit");
    }

    std::uint64_t bytes = 0;
    std::uint64_t keyFrames = 0;
    for (const auto& nalUnit : nalUnits) {
        if (nalUnit.empty()) {
            return Status::invalidArgument("cannot push H.264 access unit with an empty NAL");
        }
        bytes += nalUnit.size();
        if ((nalUnit.front() & 0x1F) == 5) {
            ++keyFrames;
        }
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        stats_.nalUnits += nalUnits.size();
        stats_.bytes += bytes;
        stats_.keyFrames += keyFrames;
    }

    auto status = pushAccessUnitToBackend(nalUnits, frameId);
    if (!status.isOk()) {
        return status;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stats_.backendAccepted += nalUnits.size();
    }
    return Status::ok();
}

Status Decoder::pushEncodedBufferWithFrameId(const std::vector<std::uint8_t>& bytes, std::uint64_t frameId) {
    rememberFrameId(frameId);
    if (!running_) {
        return Status::invalidState("cannot push encoded buffer while decoder is stopped");
    }
    if (bytes.empty()) {
        return Status::invalidArgument("cannot push empty encoded buffer");
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++stats_.nalUnits;
        stats_.bytes += bytes.size();
    }
    auto status = pushEncodedBufferToBackend(bytes, frameId);
    if (!status.isOk()) {
        return status;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++stats_.backendAccepted;
    }
    return Status::ok();
}

void Decoder::drainForMs(unsigned int milliseconds) {
    if (milliseconds == 0) {
        return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

Status Decoder::stop() {
    running_ = false;
    stopBackend();
    logInfo("client decoder stopped");
    return Status::ok();
}

DecoderStats Decoder::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

const char* Decoder::backendName() const {
#if defined(LED_HAS_GSTREAMER)
    return "gstreamer-appsrc-fakesink";
#else
    return "stats-sink";
#endif
}

const std::string& Decoder::sinkPipeline() const {
    return sinkPipeline_;
}

void Decoder::noteDecodedFrame(const RawFrameInfo& frame) {
    noteDecodedFrameData(frame, nullptr);
}

void Decoder::noteDecodedFrameData(const RawFrameInfo& frame, const std::uint8_t* data) {
    const auto frameId = takeDecodedFrameId(frame.ptsNs);
    std::function<void(std::uint64_t)> callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++stats_.decodedFrames;
        callback = renderedFrameCallback_;
    }
    if (renderer_ != nullptr) {
        if (data != nullptr) {
            renderer_->submitRawFrameData(frame, data);
        } else {
            renderer_->submitRawFrame(frame);
        }
    }
    if (callback && frameId != 0) {
        callback(frameId);
    }
}

void Decoder::rememberFrameId(std::uint64_t frameId) {
    if (frameId == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (lastQueuedFrameId_ == frameId) {
        return;
    }
    pendingFrameIds_.push_back(frameId);
    lastQueuedFrameId_ = frameId;
    while (pendingFrameIds_.size() > 240) {
        pendingFrameIds_.pop_front();
    }
}

std::uint64_t Decoder::takeDecodedFrameId(std::uint64_t ptsNs) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (ptsNs != 0 && (ptsNs % kFrameIdPtsUnitNs) == 0) {
        const auto frameId = ptsNs / kFrameIdPtsUnitNs;
        if (frameId != 0 && frameId <= lastQueuedFrameId_) {
            while (!pendingFrameIds_.empty() && pendingFrameIds_.front() <= frameId) {
                pendingFrameIds_.pop_front();
            }
            return frameId;
        }
    }
    if (pendingFrameIds_.empty()) {
        return 0;
    }
    const auto frameId = pendingFrameIds_.front();
    pendingFrameIds_.pop_front();
    return frameId;
}

std::vector<DecoderFactoryInfo> Decoder::availableGstreamerFactories() {
    std::vector<DecoderFactoryInfo> factories;
#if defined(LED_HAS_GSTREAMER)
    static bool gstInitialized = false;
    if (!gstInitialized) {
        gst_init(nullptr, nullptr);
        gstInitialized = true;
    }

    auto* registry = gst_registry_get();
    auto* features = gst_registry_get_feature_list(registry, GST_TYPE_ELEMENT_FACTORY);
    for (auto* item = features; item != nullptr; item = item->next) {
        auto* feature = GST_PLUGIN_FEATURE(item->data);
        auto* factory = GST_ELEMENT_FACTORY(feature);
        const char* klass = gst_element_factory_get_metadata(factory, GST_ELEMENT_METADATA_KLASS);
        const char* description = gst_element_factory_get_metadata(factory, GST_ELEMENT_METADATA_DESCRIPTION);
        const auto name = gst_plugin_feature_get_name(feature);
        const std::string klassText = klass != nullptr ? klass : "";
        const std::string descriptionText = description != nullptr ? description : "";
        const std::string nameText = name != nullptr ? name : "";
        const bool interesting =
            klassText.find("Decoder") != std::string::npos ||
            klassText.find("Sink/Video") != std::string::npos ||
            nameText.find("h264") != std::string::npos ||
            descriptionText.find("H.264") != std::string::npos ||
            descriptionText.find("h264") != std::string::npos;
        if (!interesting) {
            continue;
        }

        factories.push_back(DecoderFactoryInfo{
            nameText,
            klassText,
            descriptionText,
            static_cast<unsigned int>(gst_plugin_feature_get_rank(feature)),
        });
    }
    gst_plugin_feature_list_free(features);
#endif
    return factories;
}

Status Decoder::startBackend() {
#if defined(LED_HAS_GSTREAMER)
    static bool gstInitialized = false;
    if (!gstInitialized) {
        gst_init(nullptr, nullptr);
        gstInitialized = true;
    }

    const std::string pipelineDescription =
        "appsrc name=src is-live=true format=time do-timestamp=true "
        "caps=" + sourceCaps_ +
        (parserPipeline_.empty() ? " ! " : " ! " + parserPipeline_ + " ! ") +
        sinkPipeline_;

    GError* error = nullptr;
    auto* pipeline = gst_parse_launch(pipelineDescription.c_str(), &error);
    if (error != nullptr) {
        const std::string message = error->message != nullptr ? error->message : "unknown GStreamer error";
        g_error_free(error);
        return Status::internalError("failed to create GStreamer pipeline: " + message);
    }
    if (pipeline == nullptr) {
        return Status::internalError("failed to create GStreamer pipeline");
    }

    auto* src = gst_bin_get_by_name(GST_BIN(pipeline), "src");
    if (src == nullptr) {
        gst_object_unref(pipeline);
        return Status::internalError("failed to find GStreamer appsrc");
    }
    g_object_set(G_OBJECT(src), "block", FALSE, "max-bytes", 512 * 1024, nullptr);
    if (g_object_class_find_property(G_OBJECT_GET_CLASS(src), "max-buffers") != nullptr) {
        g_object_set(G_OBJECT(src), "max-buffers", 2, nullptr);
    }
    if (g_object_class_find_property(G_OBJECT_GET_CLASS(src), "leaky-type") != nullptr) {
        g_object_set(G_OBJECT(src), "leaky-type", 2, nullptr);
    }

    auto* decodedSink = gst_bin_get_by_name(GST_BIN(pipeline), "decoded_sink");
    if (decodedSink != nullptr) {
        g_signal_connect(decodedSink, "handoff", G_CALLBACK(onDecodedFrameHandoff), this);
    }

    const auto stateResult = gst_element_set_state(GST_ELEMENT(pipeline), GST_STATE_PLAYING);
    if (stateResult == GST_STATE_CHANGE_FAILURE) {
        gst_object_unref(src);
        gst_object_unref(pipeline);
        return Status::internalError("failed to start GStreamer pipeline");
    }

    pipeline_ = pipeline;
    appsrc_ = src;
    decodedSink_ = decodedSink;
    return Status::ok();
#else
    return Status::ok();
#endif
}

Status Decoder::pushNalToBackend(const std::vector<std::uint8_t>& nalUnit) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(nalUnit.size() + 4);
    bytes.push_back(0x00);
    bytes.push_back(0x00);
    bytes.push_back(0x00);
    bytes.push_back(0x01);
    bytes.insert(bytes.end(), nalUnit.begin(), nalUnit.end());
    return pushEncodedBufferToBackend(bytes, 0);
}

Status Decoder::pushEncodedBufferToBackend(const std::vector<std::uint8_t>& bytes, std::uint64_t frameId) {
#if defined(LED_HAS_GSTREAMER)
    if (appsrc_ == nullptr) {
        return Status::invalidState("GStreamer appsrc is not started");
    }

    auto* buffer = gst_buffer_new_allocate(nullptr, bytes.size(), nullptr);
    if (buffer == nullptr) {
        return Status::internalError("failed to allocate GStreamer buffer");
    }

    GstMapInfo map{};
    if (!gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
        gst_buffer_unref(buffer);
        return Status::internalError("failed to map GStreamer buffer");
    }

    std::copy(bytes.begin(), bytes.end(), map.data);
    gst_buffer_unmap(buffer, &map);
    if (frameId != 0) {
        GST_BUFFER_PTS(buffer) = static_cast<GstClockTime>(frameId * kFrameIdPtsUnitNs);
        GST_BUFFER_DTS(buffer) = GST_BUFFER_PTS(buffer);
        GST_BUFFER_DURATION(buffer) = static_cast<GstClockTime>(kFrameIdPtsUnitNs);
    }

    GstFlowReturn result = GST_FLOW_ERROR;
    g_signal_emit_by_name(G_OBJECT(appsrc_), "push-buffer", buffer, &result);
    gst_buffer_unref(buffer);
    if (result != GST_FLOW_OK) {
        return Status::internalError("GStreamer appsrc rejected buffer");
    }
#else
    (void)bytes;
    (void)frameId;
#endif
    return Status::ok();
}

Status Decoder::pushAccessUnitToBackend(
    const std::vector<std::vector<std::uint8_t>>& nalUnits,
    std::uint64_t frameId) {
#if defined(LED_HAS_GSTREAMER)
    if (appsrc_ == nullptr) {
        return Status::invalidState("GStreamer appsrc is not started");
    }

    std::size_t totalBytes = 0;
    for (const auto& nalUnit : nalUnits) {
        totalBytes += nalUnit.size() + 4;
    }
    auto* buffer = gst_buffer_new_allocate(nullptr, totalBytes, nullptr);
    if (buffer == nullptr) {
        return Status::internalError("failed to allocate GStreamer access-unit buffer");
    }

    GstMapInfo map{};
    if (!gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
        gst_buffer_unref(buffer);
        return Status::internalError("failed to map GStreamer access-unit buffer");
    }

    std::size_t offset = 0;
    for (const auto& nalUnit : nalUnits) {
        map.data[offset++] = 0x00;
        map.data[offset++] = 0x00;
        map.data[offset++] = 0x00;
        map.data[offset++] = 0x01;
        std::copy(nalUnit.begin(), nalUnit.end(), map.data + offset);
        offset += nalUnit.size();
    }
    gst_buffer_unmap(buffer, &map);
    if (frameId != 0) {
        GST_BUFFER_PTS(buffer) = static_cast<GstClockTime>(frameId * kFrameIdPtsUnitNs);
        GST_BUFFER_DTS(buffer) = GST_BUFFER_PTS(buffer);
        GST_BUFFER_DURATION(buffer) = static_cast<GstClockTime>(kFrameIdPtsUnitNs);
    }

    GstFlowReturn result = GST_FLOW_ERROR;
    g_signal_emit_by_name(G_OBJECT(appsrc_), "push-buffer", buffer, &result);
    gst_buffer_unref(buffer);
    if (result != GST_FLOW_OK) {
        return Status::internalError("GStreamer appsrc rejected access-unit buffer");
    }
#else
    (void)nalUnits;
    (void)frameId;
#endif
    return Status::ok();
}

void Decoder::stopBackend() {
#if defined(LED_HAS_GSTREAMER)
    if (appsrc_ != nullptr) {
        GstFlowReturn result = GST_FLOW_OK;
        g_signal_emit_by_name(G_OBJECT(appsrc_), "end-of-stream", &result);
    }
    if (pipeline_ != nullptr) {
        gst_element_set_state(GST_ELEMENT(pipeline_), GST_STATE_NULL);
    }
    if (decodedSink_ != nullptr) {
        g_signal_handlers_disconnect_by_data(G_OBJECT(decodedSink_), this);
        gst_object_unref(GST_OBJECT(decodedSink_));
        decodedSink_ = nullptr;
    }
    if (appsrc_ != nullptr) {
        gst_object_unref(GST_OBJECT(appsrc_));
        appsrc_ = nullptr;
    }
    if (pipeline_ != nullptr) {
        gst_object_unref(GST_OBJECT(pipeline_));
        pipeline_ = nullptr;
    }
#endif
}

}  // namespace led::client
