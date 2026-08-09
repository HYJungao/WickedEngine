#include "NewPipelineWebRTCAppleCodecFactory.h"

#if defined(__APPLE__)

#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <VideoToolbox/VideoToolbox.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "api/video/i420_buffer.h"
#include "api/environment/environment_factory.h"
#include "api/video_codecs/video_decoder_factory_template.h"
#include "api/video_codecs/video_decoder_factory_template_libvpx_vp8_adapter.h"
#include "api/video_codecs/video_encoder_factory_template.h"
#include "api/video_codecs/video_encoder_factory_template_libvpx_vp8_adapter.h"
#include "libyuv/libyuv/convert.h"
#include "libyuv/libyuv/convert_from.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "modules/video_coding/include/video_error_codes.h"

namespace
{
constexpr uint8_t kAnnexBStartCode[] = {0, 0, 0, 1};

void HardwareProbeCallback(
    void*, void*, OSStatus, VTEncodeInfoFlags, CMSampleBufferRef)
{
}

bool HardwareH264EncoderAvailable()
{
    static const bool available = [] {
        CFMutableDictionaryRef encoder_spec = CFDictionaryCreateMutable(
            kCFAllocatorDefault, 1, &kCFTypeDictionaryKeyCallBacks,
            &kCFTypeDictionaryValueCallBacks);
        CFDictionarySetValue(encoder_spec,
            kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder,
            kCFBooleanTrue);
        VTCompressionSessionRef session = nullptr;
        const OSStatus status = VTCompressionSessionCreate(kCFAllocatorDefault,
            16, 16, kCMVideoCodecType_H264, encoder_spec, nullptr, nullptr,
            &HardwareProbeCallback, nullptr, &session);
        CFRelease(encoder_spec);
        if (session != nullptr)
        {
            VTCompressionSessionInvalidate(session);
            CFRelease(session);
        }
        return status == noErr;
    }();
    return available;
}

bool HardwareH264DecoderAvailable()
{
    static const bool available =
        VTIsHardwareDecodeSupported(kCMVideoCodecType_H264);
    return available;
}

bool IsH264(const webrtc::SdpVideoFormat& format)
{
    std::string name = format.name;
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    return name == "h264";
}

void SetSessionInt(VTCompressionSessionRef session, CFStringRef key, int64_t value)
{
    CFNumberRef number = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &value);
    if (number != nullptr)
    {
        VTSessionSetProperty(session, key, number);
        CFRelease(number);
    }
}

void AppendAnnexBNal(std::vector<uint8_t>& output, const uint8_t* data, size_t size)
{
    output.insert(output.end(), std::begin(kAnnexBStartCode), std::end(kAnnexBStartCode));
    output.insert(output.end(), data, data + size);
}

bool IsKeySample(CMSampleBufferRef sample)
{
    CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample, false);
    if (attachments == nullptr || CFArrayGetCount(attachments) == 0)
        return true;
    CFDictionaryRef attachment = static_cast<CFDictionaryRef>(
        CFArrayGetValueAtIndex(attachments, 0));
    return !CFDictionaryContainsKey(attachment, kCMSampleAttachmentKey_NotSync);
}

bool SampleToAnnexB(CMSampleBufferRef sample, bool keyframe, std::vector<uint8_t>& output)
{
    output.clear();
    CMFormatDescriptionRef format = CMSampleBufferGetFormatDescription(sample);
    size_t nal_length_bytes = 4;
    if (keyframe && format != nullptr)
    {
        size_t parameter_count = 0;
        const uint8_t* parameter = nullptr;
        size_t parameter_size = 0;
        int header_size = 0;
        OSStatus status = CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
            format, 0, &parameter, &parameter_size, &parameter_count, &header_size);
        if (status == noErr)
        {
            nal_length_bytes = static_cast<size_t>(header_size);
            for (size_t index = 0; index < parameter_count; ++index)
            {
                if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
                        format, index, &parameter, &parameter_size, nullptr, nullptr) == noErr)
                    AppendAnnexBNal(output, parameter, parameter_size);
            }
        }
    }

    CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample);
    if (block == nullptr || nal_length_bytes == 0 || nal_length_bytes > 4)
        return false;
    const size_t size = CMBlockBufferGetDataLength(block);
    std::vector<uint8_t> avcc(size);
    if (size == 0 || CMBlockBufferCopyDataBytes(block, 0, size, avcc.data()) != kCMBlockBufferNoErr)
        return false;
    size_t cursor = 0;
    while (cursor + nal_length_bytes <= avcc.size())
    {
        uint32_t nal_size = 0;
        for (size_t index = 0; index < nal_length_bytes; ++index)
            nal_size = (nal_size << 8u) | avcc[cursor + index];
        cursor += nal_length_bytes;
        if (nal_size == 0 || cursor + nal_size > avcc.size())
            return false;
        AppendAnnexBNal(output, avcc.data() + cursor, nal_size);
        cursor += nal_size;
    }
    return cursor == avcc.size() && !output.empty();
}

struct EncoderFrameContext
{
    uint32_t rtp_timestamp = 0;
    int64_t timestamp_usec = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

class VideoToolboxH264Encoder final : public webrtc::VideoEncoder
{
public:
    explicit VideoToolboxH264Encoder(
        std::shared_ptr<NPHardwareEncoderFailureSignal> failure) :
        failure_(std::move(failure))
    {
    }

    ~VideoToolboxH264Encoder() override { Release(); }

    int InitEncode(
        const webrtc::VideoCodec* settings,
        const webrtc::VideoEncoder::Settings&) override
    {
        Release();
        if (settings == nullptr || settings->codecType != webrtc::kVideoCodecH264 ||
            settings->width == 0 || settings->height == 0 ||
            settings->numberOfSimulcastStreams > 1)
            return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;

        width_ = settings->width;
        height_ = settings->height;
        bitrate_bps_ = static_cast<uint32_t>(std::max(1u, settings->startBitrate)) * 1000u;
        framerate_ = std::max(1u, settings->maxFramerate);
        CFMutableDictionaryRef encoder_spec = CFDictionaryCreateMutable(
            kCFAllocatorDefault, 1, &kCFTypeDictionaryKeyCallBacks,
            &kCFTypeDictionaryValueCallBacks);
        CFDictionarySetValue(encoder_spec,
            kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder,
            kCFBooleanTrue);
        const OSStatus status = VTCompressionSessionCreate(
            kCFAllocatorDefault, width_, height_, kCMVideoCodecType_H264,
            encoder_spec, nullptr, nullptr, &CompressionOutputCallback, this, &session_);
        CFRelease(encoder_spec);
        if (status != noErr || session_ == nullptr)
            return HardwareFailure();

        VTSessionSetProperty(session_, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
        VTSessionSetProperty(session_, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);
        VTSessionSetProperty(session_, kVTCompressionPropertyKey_ProfileLevel,
            kVTProfileLevel_H264_ConstrainedBaseline_AutoLevel);
        SetSessionInt(session_, kVTCompressionPropertyKey_AverageBitRate, bitrate_bps_);
        SetSessionInt(session_, kVTCompressionPropertyKey_ExpectedFrameRate, framerate_);
        SetSessionInt(session_, kVTCompressionPropertyKey_MaxKeyFrameInterval,
            std::max<uint32_t>(framerate_ * 2u, 1u));
        if (VTCompressionSessionPrepareToEncodeFrames(session_) != noErr)
            return HardwareFailure();
        initialized_ = true;
        return WEBRTC_VIDEO_CODEC_OK;
    }

    int32_t RegisterEncodeCompleteCallback(
        webrtc::EncodedImageCallback* callback) override
    {
        std::lock_guard lock(mutex_);
        callback_ = callback;
        return WEBRTC_VIDEO_CODEC_OK;
    }

    int32_t Release() override
    {
        VTCompressionSessionRef session = session_;
        session_ = nullptr;
        initialized_ = false;
        if (session != nullptr)
        {
            VTCompressionSessionCompleteFrames(session, kCMTimeInvalid);
            VTCompressionSessionInvalidate(session);
            CFRelease(session);
        }
        std::lock_guard lock(mutex_);
        callback_ = nullptr;
        return WEBRTC_VIDEO_CODEC_OK;
    }

    int32_t Encode(
        const webrtc::VideoFrame& frame,
        const std::vector<webrtc::VideoFrameType>* frame_types) override
    {
        if (!initialized_ || session_ == nullptr)
            return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
        const auto i420 = frame.video_frame_buffer()->ToI420();
        if (!i420 || i420->width() != static_cast<int>(width_) ||
            i420->height() != static_cast<int>(height_))
            return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;

        CVPixelBufferRef pixel_buffer = nullptr;
        const CFDictionaryKeyCallBacks* key_callbacks = &kCFTypeDictionaryKeyCallBacks;
        const CFDictionaryValueCallBacks* value_callbacks = &kCFTypeDictionaryValueCallBacks;
        CFMutableDictionaryRef attributes = CFDictionaryCreateMutable(
            kCFAllocatorDefault, 1, key_callbacks, value_callbacks);
        CFDictionaryRef empty_surface_properties = CFDictionaryCreate(
            kCFAllocatorDefault, nullptr, nullptr, 0, key_callbacks, value_callbacks);
        CFDictionarySetValue(attributes, kCVPixelBufferIOSurfacePropertiesKey,
            empty_surface_properties);
        const CVReturn create_status = CVPixelBufferCreate(kCFAllocatorDefault,
            width_, height_, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
            attributes, &pixel_buffer);
        CFRelease(empty_surface_properties);
        CFRelease(attributes);
        if (create_status != kCVReturnSuccess || pixel_buffer == nullptr)
            return WEBRTC_VIDEO_CODEC_MEMORY;

        CVPixelBufferLockBaseAddress(pixel_buffer, 0);
        const int convert_status = libyuv::I420ToNV12(
            i420->DataY(), i420->StrideY(),
            i420->DataU(), i420->StrideU(),
            i420->DataV(), i420->StrideV(),
            static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pixel_buffer, 0)),
            static_cast<int>(CVPixelBufferGetBytesPerRowOfPlane(pixel_buffer, 0)),
            static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pixel_buffer, 1)),
            static_cast<int>(CVPixelBufferGetBytesPerRowOfPlane(pixel_buffer, 1)),
            width_, height_);
        CVPixelBufferUnlockBaseAddress(pixel_buffer, 0);
        if (convert_status != 0)
        {
            CVPixelBufferRelease(pixel_buffer);
            return WEBRTC_VIDEO_CODEC_ERROR;
        }

        bool force_keyframe = false;
        if (frame_types != nullptr)
        {
            force_keyframe = std::find(frame_types->begin(), frame_types->end(),
                webrtc::VideoFrameType::kVideoFrameKey) != frame_types->end();
        }
        CFDictionaryRef frame_properties = nullptr;
        if (force_keyframe)
        {
            const void* keys[] = {kVTEncodeFrameOptionKey_ForceKeyFrame};
            const void* values[] = {kCFBooleanTrue};
            frame_properties = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 1,
                &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        }
        auto context = std::make_unique<EncoderFrameContext>();
        context->rtp_timestamp = frame.rtp_timestamp();
        context->timestamp_usec = frame.timestamp_us();
        context->width = width_;
        context->height = height_;
        const CMTime timestamp = CMTimeMake(frame.timestamp_us(), 1'000'000);
        const OSStatus status = VTCompressionSessionEncodeFrame(session_, pixel_buffer,
            timestamp, kCMTimeInvalid, frame_properties, context.get(), nullptr);
        if (frame_properties != nullptr)
            CFRelease(frame_properties);
        CVPixelBufferRelease(pixel_buffer);
        if (status != noErr)
            return HardwareFailure();
        context.release();
        return WEBRTC_VIDEO_CODEC_OK;
    }

    void SetRates(const RateControlParameters& parameters) override
    {
        if (session_ == nullptr)
            return;
        const uint32_t bitrate = parameters.bitrate.get_sum_bps();
        if (bitrate > 0)
        {
            bitrate_bps_ = bitrate;
            SetSessionInt(session_, kVTCompressionPropertyKey_AverageBitRate, bitrate_bps_);
        }
        if (parameters.framerate_fps > 0.0)
        {
            framerate_ = static_cast<uint32_t>(parameters.framerate_fps + 0.5);
            SetSessionInt(session_, kVTCompressionPropertyKey_ExpectedFrameRate,
                std::max(1u, framerate_));
        }
    }

    EncoderInfo GetEncoderInfo() const override
    {
        EncoderInfo info;
        info.implementation_name = "VideoToolbox H264";
        info.is_hardware_accelerated = true;
        info.supports_native_handle = false;
        info.requested_resolution_alignment = 2;
        info.apply_alignment_to_all_simulcast_layers = false;
        info.has_trusted_rate_controller = false;
        return info;
    }

private:
    int HardwareFailure()
    {
        if (failure_)
            failure_->failed.store(true, std::memory_order_release);
        return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
    }

    static void CompressionOutputCallback(
        void* output_refcon,
        void* source_refcon,
        OSStatus status,
        VTEncodeInfoFlags,
        CMSampleBufferRef sample)
    {
        std::unique_ptr<EncoderFrameContext> context(
            static_cast<EncoderFrameContext*>(source_refcon));
        auto* encoder = static_cast<VideoToolboxH264Encoder*>(output_refcon);
        if (encoder == nullptr || context == nullptr || status != noErr || sample == nullptr ||
            !CMSampleBufferDataIsReady(sample))
        {
            if (encoder != nullptr)
                encoder->HardwareFailure();
            return;
        }
        const bool keyframe = IsKeySample(sample);
        std::vector<uint8_t> annex_b;
        if (!SampleToAnnexB(sample, keyframe, annex_b))
        {
            encoder->HardwareFailure();
            return;
        }
        webrtc::EncodedImage image;
        image.SetEncodedData(webrtc::EncodedImageBuffer::Create(
            annex_b.data(), annex_b.size()));
        image.SetRtpTimestamp(context->rtp_timestamp);
        image.capture_time_ms_ = context->timestamp_usec / 1000;
        image._encodedWidth = context->width;
        image._encodedHeight = context->height;
        image.SetFrameType(keyframe
            ? webrtc::VideoFrameType::kVideoFrameKey
            : webrtc::VideoFrameType::kVideoFrameDelta);
        webrtc::CodecSpecificInfo codec_info;
        codec_info.codecType = webrtc::kVideoCodecH264;
        codec_info.codecSpecific.H264.packetization_mode =
            webrtc::H264PacketizationMode::NonInterleaved;
        codec_info.codecSpecific.H264.temporal_idx = 0;
        codec_info.codecSpecific.H264.base_layer_sync = false;
        codec_info.codecSpecific.H264.idr_frame = keyframe;
        webrtc::EncodedImageCallback* callback = nullptr;
        {
            std::lock_guard lock(encoder->mutex_);
            callback = encoder->callback_;
        }
        if (callback != nullptr)
            callback->OnEncodedImage(image, &codec_info);
    }

    std::shared_ptr<NPHardwareEncoderFailureSignal> failure_;
    mutable std::mutex mutex_;
    webrtc::EncodedImageCallback* callback_ = nullptr;
    VTCompressionSessionRef session_ = nullptr;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t bitrate_bps_ = 1'000'000;
    uint32_t framerate_ = 30;
    bool initialized_ = false;
};

struct NalUnit
{
    const uint8_t* data = nullptr;
    size_t size = 0;
};

std::vector<NalUnit> SplitAnnexB(const uint8_t* data, size_t size)
{
    std::vector<NalUnit> units;
    const auto start_code = [data, size](size_t offset, size_t& length) {
        if (offset + 3 <= size && data[offset] == 0 && data[offset + 1] == 0)
        {
            if (data[offset + 2] == 1)
            {
                length = 3;
                return true;
            }
            if (offset + 4 <= size && data[offset + 2] == 0 && data[offset + 3] == 1)
            {
                length = 4;
                return true;
            }
        }
        return false;
    };
    size_t cursor = 0;
    while (cursor < size)
    {
        size_t prefix = 0;
        while (cursor < size && !start_code(cursor, prefix))
            ++cursor;
        if (cursor == size)
            break;
        const size_t nal_begin = cursor + prefix;
        size_t next = nal_begin;
        size_t next_prefix = 0;
        while (next < size && !start_code(next, next_prefix))
            ++next;
        if (next > nal_begin)
            units.push_back({data + nal_begin, next - nal_begin});
        cursor = next;
    }
    return units;
}

struct DecoderFrameContext
{
    uint32_t rtp_timestamp = 0;
    int64_t timestamp_usec = 0;
};

class VideoToolboxH264Decoder final : public webrtc::VideoDecoder
{
public:
    ~VideoToolboxH264Decoder() override { Release(); }

    bool Configure(const Settings&) override { return true; }

    int32_t Decode(const webrtc::EncodedImage& input, int64_t) override
    {
        const std::vector<NalUnit> units = SplitAnnexB(input.data(), input.size());
        if (units.empty())
            return WEBRTC_VIDEO_CODEC_ERROR;
        std::vector<uint8_t> new_sps;
        std::vector<uint8_t> new_pps;
        std::vector<uint8_t> avcc;
        for (const NalUnit& unit : units)
        {
            if (unit.size == 0)
                continue;
            const uint8_t type = unit.data[0] & 0x1fu;
            if (type == 7)
                new_sps.assign(unit.data, unit.data + unit.size);
            else if (type == 8)
                new_pps.assign(unit.data, unit.data + unit.size);
            const uint32_t size = static_cast<uint32_t>(unit.size);
            avcc.push_back(static_cast<uint8_t>((size >> 24u) & 0xffu));
            avcc.push_back(static_cast<uint8_t>((size >> 16u) & 0xffu));
            avcc.push_back(static_cast<uint8_t>((size >> 8u) & 0xffu));
            avcc.push_back(static_cast<uint8_t>(size & 0xffu));
            avcc.insert(avcc.end(), unit.data, unit.data + unit.size);
        }
        if (!new_sps.empty() && !new_pps.empty() &&
            (new_sps != sps_ || new_pps != pps_ || session_ == nullptr))
        {
            if (!CreateSession(new_sps, new_pps))
                return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
        }
        if (session_ == nullptr || format_ == nullptr || avcc.empty())
            return WEBRTC_VIDEO_CODEC_ERROR;

        CMBlockBufferRef block = nullptr;
        if (CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, nullptr,
                avcc.size(), kCFAllocatorDefault, nullptr, 0, avcc.size(), 0, &block) != noErr ||
            block == nullptr)
            return WEBRTC_VIDEO_CODEC_MEMORY;
        if (CMBlockBufferReplaceDataBytes(avcc.data(), block, 0, avcc.size()) != noErr)
        {
            CFRelease(block);
            return WEBRTC_VIDEO_CODEC_ERROR;
        }
        CMSampleBufferRef sample = nullptr;
        const size_t sample_size = avcc.size();
        const OSStatus sample_status = CMSampleBufferCreateReady(kCFAllocatorDefault,
            block, format_, 1, 0, nullptr, 1, &sample_size, &sample);
        CFRelease(block);
        if (sample_status != noErr || sample == nullptr)
            return WEBRTC_VIDEO_CODEC_ERROR;

        auto context = std::make_unique<DecoderFrameContext>();
        context->rtp_timestamp = input.RtpTimestamp();
        context->timestamp_usec = input.capture_time_ms_ * 1000;
        VTDecodeFrameFlags flags = kVTDecodeFrame_EnableAsynchronousDecompression |
            kVTDecodeFrame_EnableTemporalProcessing;
        const OSStatus decode_status = VTDecompressionSessionDecodeFrame(
            session_, sample, flags, context.get(), nullptr);
        CFRelease(sample);
        if (decode_status != noErr)
            return WEBRTC_VIDEO_CODEC_ERROR;
        context.release();
        return WEBRTC_VIDEO_CODEC_OK;
    }

    int32_t RegisterDecodeCompleteCallback(
        webrtc::DecodedImageCallback* callback) override
    {
        std::lock_guard lock(mutex_);
        callback_ = callback;
        return WEBRTC_VIDEO_CODEC_OK;
    }

    int32_t Release() override
    {
        if (session_ != nullptr)
        {
            VTDecompressionSessionWaitForAsynchronousFrames(session_);
            VTDecompressionSessionInvalidate(session_);
            CFRelease(session_);
            session_ = nullptr;
        }
        if (format_ != nullptr)
        {
            CFRelease(format_);
            format_ = nullptr;
        }
        sps_.clear();
        pps_.clear();
        std::lock_guard lock(mutex_);
        callback_ = nullptr;
        return WEBRTC_VIDEO_CODEC_OK;
    }

    DecoderInfo GetDecoderInfo() const override
    {
        DecoderInfo info;
        info.implementation_name = "VideoToolbox H264";
        info.is_hardware_accelerated = true;
        return info;
    }

private:
    bool CreateSession(const std::vector<uint8_t>& sps, const std::vector<uint8_t>& pps)
    {
        if (session_ != nullptr)
        {
            VTDecompressionSessionWaitForAsynchronousFrames(session_);
            VTDecompressionSessionInvalidate(session_);
            CFRelease(session_);
            session_ = nullptr;
        }
        if (format_ != nullptr)
        {
            CFRelease(format_);
            format_ = nullptr;
        }
        const uint8_t* parameter_sets[] = {sps.data(), pps.data()};
        const size_t parameter_sizes[] = {sps.size(), pps.size()};
        if (CMVideoFormatDescriptionCreateFromH264ParameterSets(kCFAllocatorDefault,
                2, parameter_sets, parameter_sizes, 4, &format_) != noErr || format_ == nullptr)
            return false;

        const int32_t pixel_format = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
        CFNumberRef pixel_format_number = CFNumberCreate(kCFAllocatorDefault,
            kCFNumberSInt32Type, &pixel_format);
        const void* destination_keys[] = {kCVPixelBufferPixelFormatTypeKey,
            kCVPixelBufferIOSurfacePropertiesKey};
        CFDictionaryRef empty = CFDictionaryCreate(kCFAllocatorDefault, nullptr, nullptr,
            0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        const void* destination_values[] = {pixel_format_number, empty};
        CFDictionaryRef destination = CFDictionaryCreate(kCFAllocatorDefault,
            destination_keys, destination_values, 2,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        const void* decoder_keys[] = {
            kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder,
            kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder};
        const void* decoder_values[] = {kCFBooleanTrue, kCFBooleanTrue};
        CFDictionaryRef decoder_spec = CFDictionaryCreate(kCFAllocatorDefault,
            decoder_keys, decoder_values, 2,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        VTDecompressionOutputCallbackRecord callback = {
            &DecompressionOutputCallback, this};
        const OSStatus status = VTDecompressionSessionCreate(kCFAllocatorDefault,
            format_, decoder_spec, destination, &callback, &session_);
        CFRelease(decoder_spec);
        CFRelease(destination);
        CFRelease(empty);
        CFRelease(pixel_format_number);
        if (status != noErr || session_ == nullptr)
            return false;
        sps_ = sps;
        pps_ = pps;
        return true;
    }

    static void DecompressionOutputCallback(
        void* output_refcon,
        void* source_refcon,
        OSStatus status,
        VTDecodeInfoFlags,
        CVImageBufferRef image_buffer,
        CMTime,
        CMTime)
    {
        std::unique_ptr<DecoderFrameContext> context(
            static_cast<DecoderFrameContext*>(source_refcon));
        auto* decoder = static_cast<VideoToolboxH264Decoder*>(output_refcon);
        if (decoder == nullptr || context == nullptr || status != noErr ||
            image_buffer == nullptr || CVPixelBufferGetPlaneCount(image_buffer) < 2)
            return;
        CVPixelBufferLockBaseAddress(image_buffer, kCVPixelBufferLock_ReadOnly);
        const int width = static_cast<int>(CVPixelBufferGetWidth(image_buffer));
        const int height = static_cast<int>(CVPixelBufferGetHeight(image_buffer));
        auto i420 = webrtc::I420Buffer::Create(width, height);
        const int convert_status = i420 ? libyuv::NV12ToI420(
            static_cast<const uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(image_buffer, 0)),
            static_cast<int>(CVPixelBufferGetBytesPerRowOfPlane(image_buffer, 0)),
            static_cast<const uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(image_buffer, 1)),
            static_cast<int>(CVPixelBufferGetBytesPerRowOfPlane(image_buffer, 1)),
            i420->MutableDataY(), i420->StrideY(),
            i420->MutableDataU(), i420->StrideU(),
            i420->MutableDataV(), i420->StrideV(), width, height) : -1;
        CVPixelBufferUnlockBaseAddress(image_buffer, kCVPixelBufferLock_ReadOnly);
        if (convert_status != 0)
            return;
        webrtc::VideoFrame frame = webrtc::VideoFrame::Builder{}
            .set_video_frame_buffer(i420)
            .set_rtp_timestamp(context->rtp_timestamp)
            .set_timestamp_us(context->timestamp_usec)
            .build();
        webrtc::DecodedImageCallback* callback = nullptr;
        {
            std::lock_guard lock(decoder->mutex_);
            callback = decoder->callback_;
        }
        if (callback != nullptr)
            callback->Decoded(frame, std::nullopt, std::nullopt);
    }

    mutable std::mutex mutex_;
    webrtc::DecodedImageCallback* callback_ = nullptr;
    VTDecompressionSessionRef session_ = nullptr;
    CMVideoFormatDescriptionRef format_ = nullptr;
    std::vector<uint8_t> sps_;
    std::vector<uint8_t> pps_;
};

class VideoToolboxEncoderFactory final : public webrtc::VideoEncoderFactory
{
public:
    explicit VideoToolboxEncoderFactory(
        std::shared_ptr<NPHardwareEncoderFailureSignal> failure) :
        failure_(std::move(failure))
    {
    }

    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override
    {
        return {webrtc::SdpVideoFormat::H264()};
    }

    CodecSupport QueryCodecSupport(
        const webrtc::SdpVideoFormat& format,
        std::optional<std::string> scalability_mode,
        std::optional<webrtc::Resolution>) const override
    {
        const bool supported = IsH264(format) && !scalability_mode.has_value() &&
            HardwareH264EncoderAvailable();
        return {supported, supported};
    }

    std::unique_ptr<webrtc::VideoEncoder> Create(
        const webrtc::Environment&,
        const webrtc::SdpVideoFormat& format) override
    {
        if (!IsH264(format))
            return nullptr;
        return std::make_unique<VideoToolboxH264Encoder>(failure_);
    }

private:
    std::shared_ptr<NPHardwareEncoderFailureSignal> failure_;
};

class VideoToolboxDecoderFactory final : public webrtc::VideoDecoderFactory
{
public:
    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override
    {
        if (!HardwareH264DecoderAvailable())
            return {};
        return {webrtc::SdpVideoFormat::H264()};
    }

    CodecSupport QueryCodecSupport(
        const webrtc::SdpVideoFormat& format,
        bool reference_scaling,
        std::optional<webrtc::Resolution>) const override
    {
        const bool supported = IsH264(format) && !reference_scaling &&
            HardwareH264DecoderAvailable();
        return {supported, supported};
    }

    std::unique_ptr<webrtc::VideoDecoder> Create(
        const webrtc::Environment&,
        const webrtc::SdpVideoFormat& format) override
    {
        return IsH264(format) && HardwareH264DecoderAvailable()
            ? std::make_unique<VideoToolboxH264Decoder>()
            : nullptr;
    }
};

bool ContainsFormat(
    const std::vector<webrtc::SdpVideoFormat>& formats,
    const webrtc::SdpVideoFormat& candidate)
{
    return std::any_of(formats.begin(), formats.end(), [&](const auto& format) {
        return format == candidate;
    });
}

class CombinedVideoEncoderFactory final : public webrtc::VideoEncoderFactory
{
public:
    void Add(std::unique_ptr<webrtc::VideoEncoderFactory> factory)
    {
        if (factory)
            factories_.push_back(std::move(factory));
    }

    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override
    {
        std::vector<webrtc::SdpVideoFormat> result;
        for (const auto& factory : factories_)
        {
            for (const auto& format : factory->GetSupportedFormats())
            {
                if (!ContainsFormat(result, format))
                    result.push_back(format);
            }
        }
        return result;
    }

    std::vector<webrtc::SdpVideoFormat> GetImplementations() const override
    {
        std::vector<webrtc::SdpVideoFormat> result;
        for (const auto& factory : factories_)
        {
            for (const auto& format : factory->GetImplementations())
            {
                if (!ContainsFormat(result, format))
                    result.push_back(format);
            }
        }
        return result;
    }

    CodecSupport QueryCodecSupport(
        const webrtc::SdpVideoFormat& format,
        std::optional<std::string> scalability_mode,
        std::optional<webrtc::Resolution> resolution) const override
    {
        for (const auto& factory : factories_)
        {
            const CodecSupport support = factory->QueryCodecSupport(
                format, scalability_mode, resolution);
            if (support.is_supported)
                return support;
        }
        return {};
    }

    std::unique_ptr<webrtc::VideoEncoder> Create(
        const webrtc::Environment& environment,
        const webrtc::SdpVideoFormat& format) override
    {
        for (const auto& factory : factories_)
        {
            if (auto encoder = factory->Create(environment, format))
                return encoder;
        }
        return nullptr;
    }

private:
    std::vector<std::unique_ptr<webrtc::VideoEncoderFactory>> factories_;
};

class CombinedVideoDecoderFactory final : public webrtc::VideoDecoderFactory
{
public:
    void Add(std::unique_ptr<webrtc::VideoDecoderFactory> factory)
    {
        if (factory)
            factories_.push_back(std::move(factory));
    }

    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override
    {
        std::vector<webrtc::SdpVideoFormat> result;
        for (const auto& factory : factories_)
        {
            for (const auto& format : factory->GetSupportedFormats())
            {
                if (!ContainsFormat(result, format))
                    result.push_back(format);
            }
        }
        return result;
    }

    std::unique_ptr<webrtc::VideoDecoder> Create(
        const webrtc::Environment& environment,
        const webrtc::SdpVideoFormat& format) override
    {
        for (const auto& factory : factories_)
        {
            if (auto decoder = factory->Create(environment, format))
                return decoder;
        }
        return nullptr;
    }

private:
    std::vector<std::unique_ptr<webrtc::VideoDecoderFactory>> factories_;
};
} // namespace

NPAppleVideoCodecFactories np_create_apple_video_codec_factories(
    bool request_hardware_encoder)
{
    NPAppleVideoCodecFactories result;
    result.hardware_failure = std::make_shared<NPHardwareEncoderFailureSignal>();
    auto encoders = std::make_unique<CombinedVideoEncoderFactory>();
    auto decoders = std::make_unique<CombinedVideoDecoderFactory>();

    if (request_hardware_encoder && HardwareH264EncoderAvailable())
    {
        result.hardware_encoder_available = true;
        encoders->Add(std::make_unique<VideoToolboxEncoderFactory>(result.hardware_failure));
    }
    decoders->Add(std::make_unique<VideoToolboxDecoderFactory>());
    encoders->Add(std::make_unique<webrtc::VideoEncoderFactoryTemplate<
        webrtc::LibvpxVp8EncoderTemplateAdapter>>());
    decoders->Add(std::make_unique<webrtc::VideoDecoderFactoryTemplate<
        webrtc::LibvpxVp8DecoderTemplateAdapter>>());
    result.encoder = std::move(encoders);
    result.decoder = std::move(decoders);
    return result;
}

extern "C" int np_validate_apple_video_codec_factories(
    char* error, size_t error_capacity)
{
    const auto fail = [error, error_capacity](const char* message) {
        if (error != nullptr && error_capacity > 0)
        {
            const size_t count = std::min(std::strlen(message), error_capacity - 1u);
            std::memcpy(error, message, count);
            error[count] = '\0';
        }
        return 0;
    };
    NPAppleVideoCodecFactories software =
        np_create_apple_video_codec_factories(false);
    if (!software.encoder || !software.decoder)
        return fail("Apple software codec factory construction failed");
    for (const webrtc::SdpVideoFormat& format :
        software.encoder->GetSupportedFormats())
    {
        if (IsH264(format))
            return fail("Forced software mode advertised an H264 encoder");
    }

    NPAppleVideoCodecFactories hardware =
        np_create_apple_video_codec_factories(true);
    if (!hardware.encoder || !hardware.decoder)
        return fail("Apple hardware codec factory construction failed");
    if (!hardware.hardware_encoder_available)
        return 1;
    const webrtc::SdpVideoFormat h264 = webrtc::SdpVideoFormat::H264();
    const auto support = hardware.encoder->QueryCodecSupport(
        h264, std::nullopt, std::nullopt);
    if (!support.is_supported || !support.is_power_efficient)
        return fail("VideoToolbox H264 capability telemetry is inconsistent");
    const webrtc::Environment environment = webrtc::CreateEnvironment();
    std::unique_ptr<webrtc::VideoEncoder> encoder =
        hardware.encoder->Create(environment, h264);
    if (!encoder)
        return fail("VideoToolbox H264 encoder creation failed");
    webrtc::VideoCodec codec;
    codec.codecType = webrtc::kVideoCodecH264;
    codec.width = 640;
    codec.height = 360;
    codec.startBitrate = 1000;
    codec.maxBitrate = 4000;
    codec.minBitrate = 100;
    codec.maxFramerate = 30;
    codec.numberOfSimulcastStreams = 1;
    const webrtc::VideoEncoder::Settings settings(
        webrtc::VideoEncoder::Capabilities(false), 1, 1200);
    const int result = encoder->InitEncode(&codec, settings);
    encoder->Release();
    if (result != WEBRTC_VIDEO_CODEC_OK)
        return fail("VideoToolbox H264 encoder initialization failed");
    return 1;
}

#endif
