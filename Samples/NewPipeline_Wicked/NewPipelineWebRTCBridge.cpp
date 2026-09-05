#include "NewPipelineWebRTCBridge.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "api/data_channel_interface.h"
#include "api/create_peerconnection_factory.h"
#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/jsep.h"
#include "api/make_ref_counted.h"
#include "api/media_stream_interface.h"
#include "api/peer_connection_interface.h"
#include "api/scoped_refptr.h"
#include "api/set_local_description_observer_interface.h"
#include "api/set_remote_description_observer_interface.h"
#include "api/stats/rtc_stats_collector_callback.h"
#include "api/stats/rtcstats_objects.h"
#include "api/task_queue/default_task_queue_factory.h"
#include "api/video/i420_buffer.h"
#include "api/video/video_frame.h"
#include "api/video/video_sink_interface.h"
#include "api/video_codecs/video_decoder_factory_template.h"
#include "api/video_codecs/video_decoder_factory_template_libvpx_vp8_adapter.h"
#include "api/video_codecs/video_encoder_factory_template.h"
#include "api/video_codecs/video_encoder_factory_template_libvpx_vp8_adapter.h"
#include "media/base/adapted_video_track_source.h"
#include "common_video/include/video_frame_buffer.h"
#include "rtc_base/logging.h"
#include "rtc_base/ssl_adapter.h"
#include "rtc_base/thread.h"

#if defined(__APPLE__)
#include "NewPipelineWebRTCAppleCodecFactory.h"
#elif defined(_WIN32)
#include "NewPipelineWebRTCWindowsCodecFactory.h"
#endif

namespace
{
constexpr size_t kMaxControlBytes = 64u * 1024u;
constexpr size_t kMaxDataChannelBufferedBytes = 1u * 1024u * 1024u;
// Keep decoded video latest-only and strictly smaller than the Windows native
// decoder's retained-surface ring (8). If both capacities are 8, a paused
// render thread lets the receive queue retain every surface, so the decoder
// cannot produce the ninth frame that would evict/release the first one.
constexpr size_t kMaxReceiveVideoQueueDepth = 3u;
// Metadata is published before the matching frame has passed through hardware
// encode, RTP jitter buffering and hardware decode. Keep more than two seconds
// at the maximum supported 60 FPS so transport latency cannot evict the exact
// SEI identity before its video frame arrives.
constexpr size_t kMaxReceiveMetadataQueueDepth = 128u;
constexpr uint32_t kMaxVideoDimension = 8192;
// The V3 video frame is a semantic data atlas rather than a camera image. Let congestion
// control drop frames, but never silently downscale it because that destroys the
// one-to-one mapping between decoded pixels and buffer samples.
constexpr int kBufferVideoMinBitrateBps = 20'000'000;
constexpr int kBufferVideoMaxBitrateBps = 120'000'000;
constexpr double kBufferVideoMaxFramerate = 30.0;

class RetainedI420Buffer : public webrtc::I420BufferInterface
{
public:
    RetainedI420Buffer(
        int width,
        int height,
        const uint8_t* y_plane,
        int y_stride,
        const uint8_t* u_plane,
        int u_stride,
        const uint8_t* v_plane,
        int v_stride,
        std::function<void()> release)
        : width_(width)
        , height_(height)
        , y_plane_(y_plane)
        , y_stride_(y_stride)
        , u_plane_(u_plane)
        , u_stride_(u_stride)
        , v_plane_(v_plane)
        , v_stride_(v_stride)
        , release_(std::move(release))
    {
    }

    int width() const override { return width_; }
    int height() const override { return height_; }
    const uint8_t* DataY() const override { return y_plane_; }
    const uint8_t* DataU() const override { return u_plane_; }
    const uint8_t* DataV() const override { return v_plane_; }
    int StrideY() const override { return y_stride_; }
    int StrideU() const override { return u_stride_; }
    int StrideV() const override { return v_stride_; }

protected:
    ~RetainedI420Buffer() override
    {
        if (release_)
            release_();
    }

private:
    int width_ = 0;
    int height_ = 0;
    const uint8_t* y_plane_ = nullptr;
    int y_stride_ = 0;
    const uint8_t* u_plane_ = nullptr;
    int u_stride_ = 0;
    const uint8_t* v_plane_ = nullptr;
    int v_stride_ = 0;
    std::function<void()> release_;
};

std::vector<std::string> SplitTokens(const std::string& value, char delimiter)
{
    std::vector<std::string> tokens;
    size_t begin = 0;
    while (begin <= value.size())
    {
        const size_t end = value.find(delimiter, begin);
        tokens.emplace_back(value.substr(begin, end == std::string::npos ? std::string::npos : end - begin));
        if (end == std::string::npos)
            break;
        begin = end + 1;
    }
    return tokens;
}

std::string JoinTokens(const std::vector<std::string>& tokens, size_t start, char delimiter)
{
    if (start >= tokens.size())
        return {};
    std::string result = tokens[start];
    for (size_t index = start + 1; index < tokens.size(); ++index)
    {
        result.push_back(delimiter);
        result += tokens[index];
    }
    return result;
}

std::string Base64Encode(std::string_view input)
{
    static constexpr char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    output.reserve(((input.size() + 2) / 3) * 4);
    size_t index = 0;
    while (index + 3 <= input.size())
    {
        const uint32_t value = (static_cast<uint8_t>(input[index]) << 16u) |
            (static_cast<uint8_t>(input[index + 1]) << 8u) | static_cast<uint8_t>(input[index + 2]);
        output.push_back(table[(value >> 18u) & 0x3fu]);
        output.push_back(table[(value >> 12u) & 0x3fu]);
        output.push_back(table[(value >> 6u) & 0x3fu]);
        output.push_back(table[value & 0x3fu]);
        index += 3;
    }
    const size_t remaining = input.size() - index;
    if (remaining == 1)
    {
        const uint32_t value = static_cast<uint8_t>(input[index]) << 16u;
        output.push_back(table[(value >> 18u) & 0x3fu]);
        output.push_back(table[(value >> 12u) & 0x3fu]);
        output += "==";
    }
    else if (remaining == 2)
    {
        const uint32_t value = (static_cast<uint8_t>(input[index]) << 16u) |
            (static_cast<uint8_t>(input[index + 1]) << 8u);
        output.push_back(table[(value >> 18u) & 0x3fu]);
        output.push_back(table[(value >> 12u) & 0x3fu]);
        output.push_back(table[(value >> 6u) & 0x3fu]);
        output.push_back('=');
    }
    return output;
}

std::optional<std::string> Base64Decode(std::string_view input)
{
    static const std::array<int, 256> reverse = [] {
        std::array<int, 256> result = {};
        result.fill(-1);
        for (int index = 0; index < 26; ++index)
        {
            result[static_cast<size_t>('A' + index)] = index;
            result[static_cast<size_t>('a' + index)] = 26 + index;
        }
        for (int index = 0; index < 10; ++index)
            result[static_cast<size_t>('0' + index)] = 52 + index;
        result[static_cast<size_t>('+')] = 62;
        result[static_cast<size_t>('/')] = 63;
        return result;
    }();
    if (input.size() % 4 != 0)
        return std::nullopt;
    std::string output;
    output.reserve(input.size() / 4 * 3);
    for (size_t index = 0; index < input.size(); index += 4)
    {
        int values[4] = {};
        int padding = 0;
        for (int component = 0; component < 4; ++component)
        {
            const char value = input[index + component];
            if (value == '=')
            {
                values[component] = 0;
                ++padding;
            }
            else
            {
                values[component] = reverse[static_cast<uint8_t>(value)];
                if (values[component] < 0)
                    return std::nullopt;
            }
        }
        const uint32_t packed = (static_cast<uint32_t>(values[0]) << 18u) |
            (static_cast<uint32_t>(values[1]) << 12u) |
            (static_cast<uint32_t>(values[2]) << 6u) | static_cast<uint32_t>(values[3]);
        output.push_back(static_cast<char>((packed >> 16u) & 0xffu));
        if (padding < 2)
            output.push_back(static_cast<char>((packed >> 8u) & 0xffu));
        if (padding < 1)
            output.push_back(static_cast<char>(packed & 0xffu));
    }
    return output;
}

struct ParsedWebSocketUrl
{
    bool secure = false;
    std::string host;
    uint16_t port = 80;
    std::string path = "/";
};

bool ParseWebSocketUrl(const std::string& url, ParsedWebSocketUrl& output)
{
    std::string remaining = url;
    if (remaining.rfind("ws://", 0) == 0)
    {
        remaining = remaining.substr(5);
        output.port = 80;
    }
    else if (remaining.rfind("wss://", 0) == 0)
    {
        remaining = remaining.substr(6);
        output.port = 443;
        output.secure = true;
    }
    else
    {
        return false;
    }
    const size_t slash = remaining.find('/');
    const std::string host_port = slash == std::string::npos ? remaining : remaining.substr(0, slash);
    output.path = slash == std::string::npos ? "/" : remaining.substr(slash);
    const size_t colon = host_port.find(':');
    output.host = colon == std::string::npos ? host_port : host_port.substr(0, colon);
    if (output.host.empty())
        return false;
    if (colon != std::string::npos)
    {
        const int port = std::atoi(host_port.substr(colon + 1).c_str());
        if (port <= 0 || port > 65535)
            return false;
        output.port = static_cast<uint16_t>(port);
    }
    return true;
}

#if defined(_WIN32)
std::wstring ToWideString(const std::string& input)
{
    if (input.empty())
        return {};
    const int size = ::MultiByteToWideChar(
        CP_UTF8, 0, input.data(), static_cast<int>(input.size()), nullptr, 0);
    if (size <= 0)
        return {};
    std::wstring output(static_cast<size_t>(size), L'\0');
    ::MultiByteToWideChar(
        CP_UTF8, 0, input.data(), static_cast<int>(input.size()), output.data(), size);
    return output;
}

class WebSocketSignalingClient
{
public:
    ~WebSocketSignalingClient() { Close(); }

    bool Connect(const std::string& url)
    {
        Close();
        ParsedWebSocketUrl parsed;
        if (!ParseWebSocketUrl(url, parsed))
            return false;

        session_ = ::WinHttpOpen(L"NewPipeline-Wicked-WebRTC/1.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session_)
            return false;
        ::WinHttpSetTimeouts(session_, 200, 250, 1000, 1000);

        const std::wstring host = ToWideString(parsed.host);
        connection_ = ::WinHttpConnect(session_, host.c_str(),
            static_cast<INTERNET_PORT>(parsed.port), 0);
        if (!connection_)
        {
            Close();
            return false;
        }

        const std::wstring path = ToWideString(parsed.path);
        request_ = ::WinHttpOpenRequest(connection_, L"GET", path.c_str(), nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
            parsed.secure ? WINHTTP_FLAG_SECURE : 0);
        if (!request_ ||
            !::WinHttpSetOption(request_, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) ||
            !::WinHttpSendRequest(request_, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !::WinHttpReceiveResponse(request_, nullptr))
        {
            Close();
            return false;
        }

        websocket_ = ::WinHttpWebSocketCompleteUpgrade(request_, 0);
        if (!websocket_)
        {
            Close();
            return false;
        }
        ::WinHttpCloseHandle(request_);
        request_ = nullptr;
        return true;
    }

    void Close()
    {
        if (websocket_)
        {
            ::WinHttpWebSocketClose(websocket_, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
            ::WinHttpCloseHandle(websocket_);
            websocket_ = nullptr;
        }
        if (request_)
        {
            ::WinHttpCloseHandle(request_);
            request_ = nullptr;
        }
        if (connection_)
        {
            ::WinHttpCloseHandle(connection_);
            connection_ = nullptr;
        }
        if (session_)
        {
            ::WinHttpCloseHandle(session_);
            session_ = nullptr;
        }
    }

    bool IsConnected() const { return websocket_ != nullptr; }

    bool SendText(const std::string& text)
    {
        if (!websocket_ || text.size() > static_cast<size_t>(std::numeric_limits<DWORD>::max()))
            return false;
        return ::WinHttpWebSocketSend(websocket_, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
            const_cast<char*>(text.data()), static_cast<DWORD>(text.size())) == NO_ERROR;
    }

    bool ReceiveText(std::string& output)
    {
        output.clear();
        if (!websocket_)
            return false;
        std::array<char, 4096> buffer = {};
        for (;;)
        {
            DWORD bytes_read = 0;
            WINHTTP_WEB_SOCKET_BUFFER_TYPE type = WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE;
            const DWORD result = ::WinHttpWebSocketReceive(websocket_, buffer.data(),
                static_cast<DWORD>(buffer.size()), &bytes_read, &type);
            if (result == ERROR_WINHTTP_TIMEOUT)
                return true;
            if (result != NO_ERROR || type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE)
                return false;
            if (type != WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE &&
                type != WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE)
                continue;
            output.append(buffer.data(), buffer.data() + bytes_read);
            if (output.size() > 16u * 1024u * 1024u)
                return false;
            if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE)
                return true;
        }
    }

private:
    HINTERNET session_ = nullptr;
    HINTERNET connection_ = nullptr;
    HINTERNET request_ = nullptr;
    HINTERNET websocket_ = nullptr;
};
#else
class WebSocketSignalingClient
{
public:
    ~WebSocketSignalingClient() { Close(); }

    bool Connect(const std::string& url)
    {
        Close();
        ParsedWebSocketUrl parsed;
        if (!ParseWebSocketUrl(url, parsed) || parsed.secure)
            return false;
        addrinfo hints = {};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* results = nullptr;
        const std::string port = std::to_string(parsed.port);
        if (::getaddrinfo(parsed.host.c_str(), port.c_str(), &hints, &results) != 0)
            return false;
        for (addrinfo* item = results; item != nullptr; item = item->ai_next)
        {
            const int socket = ::socket(item->ai_family, item->ai_socktype, item->ai_protocol);
            if (socket < 0)
                continue;
            SetSocketTimeouts(socket);
            if (::connect(socket, item->ai_addr, static_cast<socklen_t>(item->ai_addrlen)) == 0)
            {
                socket_ = socket;
                break;
            }
            ::close(socket);
        }
        ::freeaddrinfo(results);
        if (socket_ < 0)
            return false;

        const std::string key = MakeClientKey();
        const std::string request = "GET " + parsed.path + " HTTP/1.1\r\nHost: " + parsed.host + ":" + port +
            "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: " + key + "\r\n\r\n";
        if (!SendAll(request.data(), request.size()))
        {
            Close();
            return false;
        }
        std::string response;
        char buffer[512] = {};
        while (response.find("\r\n\r\n") == std::string::npos && response.size() < 8192)
        {
            const ssize_t bytes = ::recv(socket_, buffer, sizeof(buffer), 0);
            if (bytes <= 0)
            {
                Close();
                return false;
            }
            response.append(buffer, buffer + bytes);
        }
        if (response.find(" 101 ") == std::string::npos && response.find(" 101\r\n") == std::string::npos)
        {
            Close();
            return false;
        }
        return true;
    }

    void Close()
    {
        const int socket = socket_.exchange(-1);
        if (socket >= 0)
        {
            ::shutdown(socket, SHUT_RDWR);
            ::close(socket);
        }
    }

    bool IsConnected() const { return socket_.load() >= 0; }

    bool SendText(const std::string& text)
    {
        return SendFrame(0x1u, reinterpret_cast<const uint8_t*>(text.data()), text.size());
    }

    bool ReceiveText(std::string& output)
    {
        output.clear();
        for (;;)
        {
            uint8_t header[2] = {};
            const ReceiveStatus status = ReceiveExact(header, sizeof(header));
            if (status == ReceiveStatus::Timeout)
                return true;
            if (status != ReceiveStatus::Complete)
                return false;
            const bool fin = (header[0] & 0x80u) != 0;
            const uint8_t opcode = header[0] & 0x0fu;
            const bool masked = (header[1] & 0x80u) != 0;
            uint64_t payload_size = header[1] & 0x7fu;
            if (payload_size == 126)
            {
                uint8_t extended[2] = {};
                if (ReceiveExact(extended, sizeof(extended)) != ReceiveStatus::Complete)
                    return false;
                payload_size = (static_cast<uint64_t>(extended[0]) << 8u) | extended[1];
            }
            else if (payload_size == 127)
            {
                uint8_t extended[8] = {};
                if (ReceiveExact(extended, sizeof(extended)) != ReceiveStatus::Complete)
                    return false;
                payload_size = 0;
                for (uint8_t value : extended)
                    payload_size = (payload_size << 8u) | value;
            }
            if (payload_size > 16u * 1024u * 1024u)
                return false;
            uint8_t mask[4] = {};
            if (masked && ReceiveExact(mask, sizeof(mask)) != ReceiveStatus::Complete)
                return false;
            std::string payload(static_cast<size_t>(payload_size), '\0');
            if (payload_size > 0 && ReceiveExact(payload.data(), payload.size()) != ReceiveStatus::Complete)
                return false;
            if (masked)
            {
                for (size_t index = 0; index < payload.size(); ++index)
                    payload[index] = static_cast<char>(static_cast<uint8_t>(payload[index]) ^ mask[index & 3u]);
            }
            if (opcode == 0x8u)
                return false;
            if (opcode == 0x9u)
            {
                SendFrame(0xau, reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
                continue;
            }
            if (opcode != 0x1u && opcode != 0x0u)
                continue;
            output += payload;
            if (fin)
                return true;
        }
    }

private:
    enum class ReceiveStatus { Complete, Timeout, Closed };

    static void SetSocketTimeouts(int socket)
    {
        timeval timeout = {};
        timeout.tv_sec = 1;
        ::setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        ::setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    }

    static std::string MakeClientKey()
    {
        static std::atomic<uint64_t> counter{0};
        const uint64_t a = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        const uint64_t b = counter.fetch_add(1) ^ 0x9e3779b97f4a7c15ull;
        std::array<char, 16> bytes = {};
        for (size_t index = 0; index < 8; ++index)
        {
            bytes[index] = static_cast<char>((a >> (index * 8u)) & 0xffu);
            bytes[index + 8] = static_cast<char>((b >> (index * 8u)) & 0xffu);
        }
        return Base64Encode(std::string_view{bytes.data(), bytes.size()});
    }

    bool SendAll(const void* data, size_t size)
    {
        const auto* bytes = static_cast<const uint8_t*>(data);
        size_t sent = 0;
        while (sent < size)
        {
            const int socket = socket_.load();
            if (socket < 0)
                return false;
            const ssize_t count = ::send(socket, bytes + sent, size - sent, 0);
            if (count <= 0)
                return false;
            sent += static_cast<size_t>(count);
        }
        return true;
    }

    ReceiveStatus ReceiveExact(void* data, size_t size)
    {
        auto* bytes = static_cast<uint8_t*>(data);
        size_t received = 0;
        while (received < size)
        {
            const int socket = socket_.load();
            if (socket < 0)
                return ReceiveStatus::Closed;
            const ssize_t count = ::recv(socket, bytes + received, size - received, 0);
            if (count == 0)
                return ReceiveStatus::Closed;
            if (count < 0)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    return received == 0 ? ReceiveStatus::Timeout : ReceiveStatus::Closed;
                return ReceiveStatus::Closed;
            }
            received += static_cast<size_t>(count);
        }
        return ReceiveStatus::Complete;
    }

    bool SendFrame(uint8_t opcode, const uint8_t* payload, size_t payload_size)
    {
        if (socket_.load() < 0)
            return false;
        static std::atomic<uint32_t> mask_counter{0x12345678u};
        std::vector<uint8_t> frame;
        frame.reserve(14 + payload_size);
        frame.push_back(static_cast<uint8_t>(0x80u | opcode));
        if (payload_size < 126)
            frame.push_back(static_cast<uint8_t>(0x80u | payload_size));
        else if (payload_size <= 0xffffu)
        {
            frame.push_back(0x80u | 126u);
            frame.push_back(static_cast<uint8_t>((payload_size >> 8u) & 0xffu));
            frame.push_back(static_cast<uint8_t>(payload_size & 0xffu));
        }
        else
        {
            frame.push_back(0x80u | 127u);
            for (int shift = 56; shift >= 0; shift -= 8)
                frame.push_back(static_cast<uint8_t>((static_cast<uint64_t>(payload_size) >> shift) & 0xffu));
        }
        const uint32_t mask_value = mask_counter.fetch_add(0x9e3779b9u);
        const uint8_t mask[4] = {
            static_cast<uint8_t>(mask_value >> 24u), static_cast<uint8_t>(mask_value >> 16u),
            static_cast<uint8_t>(mask_value >> 8u), static_cast<uint8_t>(mask_value)};
        frame.insert(frame.end(), std::begin(mask), std::end(mask));
        for (size_t index = 0; index < payload_size; ++index)
            frame.push_back(payload[index] ^ mask[index & 3u]);
        return SendAll(frame.data(), frame.size());
    }

    std::atomic<int> socket_{-1};
};
#endif

struct ReceivedVideoFrame
{
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t rtp_timestamp = 0;
    int64_t timestamp_usec = 0;
    webrtc::scoped_refptr<webrtc::VideoFrameBuffer> buffer;
};

struct CodecTelemetry
{
    std::atomic<uint64_t> compressed_bytes_sent{0};
    std::atomic<uint64_t> compressed_bytes_received{0};
    std::atomic<uint64_t> total_encode_time_usec{0};
    std::atomic<uint64_t> total_decode_time_usec{0};
    std::atomic<uint64_t> frames_encoded{0};
    std::atomic<uint64_t> frames_decoded{0};
    std::atomic_bool power_efficient{false};
    std::mutex text_mutex;
    std::string codec_name = "unknown";
    std::string implementation = "unknown";
    std::string codec_profile = "unknown";
};

class CodecStatsCallback : public webrtc::RTCStatsCollectorCallback
{
public:
    explicit CodecStatsCallback(std::shared_ptr<CodecTelemetry> telemetry) : telemetry_(std::move(telemetry)) {}

    void OnStatsDelivered(const webrtc::scoped_refptr<const webrtc::RTCStatsReport>& report) override
    {
        if (!report || !telemetry_)
            return;
        std::string codec_id;
        std::string implementation;
        bool power_efficient = false;
        for (const webrtc::RTCOutboundRtpStreamStats* stats :
            report->GetStatsOfType<webrtc::RTCOutboundRtpStreamStats>())
        {
            if (!stats->kind || *stats->kind != "video")
                continue;
            telemetry_->compressed_bytes_sent.store(stats->bytes_sent.value_or(0), std::memory_order_relaxed);
            telemetry_->total_encode_time_usec.store(
                static_cast<uint64_t>(stats->total_encode_time.value_or(0.0) * 1'000'000.0),
                std::memory_order_relaxed);
            telemetry_->frames_encoded.store(stats->frames_encoded.value_or(0), std::memory_order_relaxed);
            if (stats->codec_id)
                codec_id = *stats->codec_id;
            if (stats->encoder_implementation)
                implementation = *stats->encoder_implementation;
            power_efficient = stats->power_efficient_encoder.value_or(false);
        }
        for (const webrtc::RTCInboundRtpStreamStats* stats :
            report->GetStatsOfType<webrtc::RTCInboundRtpStreamStats>())
        {
            if (!stats->kind || *stats->kind != "video")
                continue;
            telemetry_->compressed_bytes_received.store(stats->bytes_received.value_or(0), std::memory_order_relaxed);
            telemetry_->total_decode_time_usec.store(
                static_cast<uint64_t>(stats->total_decode_time.value_or(0.0) * 1'000'000.0),
                std::memory_order_relaxed);
            telemetry_->frames_decoded.store(stats->frames_decoded.value_or(0), std::memory_order_relaxed);
            if (stats->codec_id)
                codec_id = *stats->codec_id;
            if (stats->decoder_implementation)
                implementation = *stats->decoder_implementation;
            power_efficient = power_efficient || stats->power_efficient_decoder.value_or(false);
        }
        std::string codec_name;
        std::string codec_profile;
        if (!codec_id.empty())
        {
            for (const webrtc::RTCCodecStats* codec : report->GetStatsOfType<webrtc::RTCCodecStats>())
            {
                if (codec->id() == codec_id && codec->mime_type)
                {
                    codec_name = *codec->mime_type;
                    codec_profile = codec->sdp_fmtp_line.value_or("unknown");
                    break;
                }
            }
        }
        telemetry_->power_efficient.store(power_efficient, std::memory_order_relaxed);
        if (!codec_name.empty() || !implementation.empty())
        {
            std::lock_guard lock(telemetry_->text_mutex);
            if (!codec_name.empty())
                telemetry_->codec_name = std::move(codec_name);
            if (!implementation.empty())
                telemetry_->implementation = std::move(implementation);
            if (!codec_profile.empty())
                telemetry_->codec_profile = std::move(codec_profile);
        }
    }

private:
    std::shared_ptr<CodecTelemetry> telemetry_;
};

class WebRTCSession final : public webrtc::PeerConnectionObserver
{
public:
    WebRTCSession(bool server, std::string signaling_url, std::string room_id,
        bool use_internet_ice, NPWebRTCEncoderPreference encoder_preference,
        uint64_t adapter_luid) :
        server_(server), signaling_url_(std::move(signaling_url)), room_id_(std::move(room_id)),
        use_internet_ice_(use_internet_ice), encoder_preference_(encoder_preference),
        adapter_luid_(adapter_luid)
    {
        if (server_)
        {
            requested_encoder_mode_ = encoder_preference_ == NP_WEBRTC_ENCODER_SOFTWARE
                ? "software" : "hardware";
            active_encoder_mode_ = encoder_preference_ == NP_WEBRTC_ENCODER_SOFTWARE
                ? "software" : "fallback-software";
            if (encoder_preference_ == NP_WEBRTC_ENCODER_SOFTWARE)
            {
                fallback_reason_ = "none";
            }
            else
            {
#if defined(__APPLE__) || defined(_WIN32)
                fallback_reason_ = "hardware-capability-probe-pending";
#else
                fallback_reason_ = "hardware-backend-not-built";
#endif
            }
        }
        else
        {
            requested_encoder_mode_ = "remote";
            active_encoder_mode_ = "decoder";
            input_surface_ = "i420-decoded";
        }
        Initialize();
    }

    ~WebRTCSession() override { Shutdown(); }

    bool IsReady() const { return initialized_.load() && !stop_requested_.load(); }
    bool IsConnected() const { return peer_connected_.load(); }

    NPWebRTCBridgeState GetState() const
    {
#if defined(__APPLE__)
        if (server_ && hardware_encoder_failure_ &&
            hardware_encoder_failure_->failed.load(std::memory_order_acquire))
            return NP_WEBRTC_FAILED;
        if (!server_ && hardware_decoder_failure_ &&
            hardware_decoder_failure_->failed.load(std::memory_order_acquire))
            return NP_WEBRTC_FAILED;
#elif defined(_WIN32)
        if (server_ && hardware_encoder_failure_ &&
            hardware_encoder_failure_->failed.load(std::memory_order_acquire))
            return NP_WEBRTC_FAILED;
        if (!server_ && hardware_decoder_failure_ &&
            hardware_decoder_failure_->failed.load(std::memory_order_acquire))
            return NP_WEBRTC_FAILED;
#endif
        if (failed_.load())
            return NP_WEBRTC_FAILED;
        if (!initialized_.load())
            return NP_WEBRTC_STARTING;
        return peer_connected_.load() ? NP_WEBRTC_CONNECTED : NP_WEBRTC_SIGNALING;
    }

    std::string GetStatus() const
    {
#if defined(__APPLE__)
        if (server_ && hardware_encoder_failure_ &&
            hardware_encoder_failure_->failed.load(std::memory_order_acquire))
            return "VideoToolbox H264 encoder failed; requesting software fallback";
        if (!server_ && hardware_decoder_failure_ &&
            hardware_decoder_failure_->failed.load(std::memory_order_acquire))
        {
            return "VideoToolbox H264 decoder failed at stage " +
                std::to_string(hardware_decoder_failure_->failure_stage.load(
                    std::memory_order_relaxed)) + " (OSStatus " +
                std::to_string(hardware_decoder_failure_->failure_status.load(
                    std::memory_order_relaxed)) +
                "); reconnecting with VP8 fallback";
        }
#elif defined(_WIN32)
        if (server_ && hardware_encoder_failure_ &&
            hardware_encoder_failure_->failed.load(std::memory_order_acquire))
        {
            return "Media Foundation H264 encoder failed at stage " +
                std::to_string(hardware_encoder_failure_->failure_stage.load(
                    std::memory_order_relaxed)) + " (HRESULT " +
                std::to_string(hardware_encoder_failure_->failure_hresult.load(
                    std::memory_order_relaxed)) + ", calls=" +
                std::to_string(hardware_encoder_failure_->encode_calls.load(
                    std::memory_order_relaxed)) + ", native=" +
                std::to_string(hardware_encoder_failure_->native_frames.load(
                    std::memory_order_relaxed)) + ", submitted=" +
                std::to_string(hardware_encoder_failure_->submitted_frames.load(
                    std::memory_order_relaxed)) + ", output=" +
                std::to_string(hardware_encoder_failure_->output_frames.load(
                    std::memory_order_relaxed)) + "); requesting VP8 fallback";
        }
        if (!server_ && hardware_decoder_failure_ &&
            hardware_decoder_failure_->failed.load(std::memory_order_acquire))
            return "Media Foundation H264 decoder failed; reconnecting with VP8 fallback";
#endif
        std::string status;
        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            status = status_;
        }
#if defined(_WIN32)
        if (server_ && hardware_encoder_failure_)
        {
            status += " [H264 enc calls=" + std::to_string(
                hardware_encoder_failure_->encode_calls.load(std::memory_order_relaxed));
            status += " native=" + std::to_string(
                hardware_encoder_failure_->native_frames.load(std::memory_order_relaxed));
            status += " submitted=" + std::to_string(
                hardware_encoder_failure_->submitted_frames.load(std::memory_order_relaxed));
            status += " output=" + std::to_string(
                hardware_encoder_failure_->output_frames.load(std::memory_order_relaxed));
            status += "]";
        }
        if (!server_ && hardware_decoder_failure_)
        {
            status += " [H264 dec calls=" + std::to_string(
                hardware_decoder_failure_->decode_calls.load(std::memory_order_relaxed));
            status += " key=" + std::to_string(
                hardware_decoder_failure_->keyframes_received.load(std::memory_order_relaxed));
            status += " queued=" + std::to_string(
                hardware_decoder_failure_->queued_frames.load(std::memory_order_relaxed));
            status += " submitted=" + std::to_string(
                hardware_decoder_failure_->submitted_frames.load(std::memory_order_relaxed));
            status += " out=" + std::to_string(
                hardware_decoder_failure_->decoded_frames.load(std::memory_order_relaxed));
            status += " nat=" + std::to_string(
                hardware_decoder_failure_->native_surface_outputs.load(std::memory_order_relaxed)) + "/" +
                std::to_string(hardware_decoder_failure_->native_surface_attempts.load(std::memory_order_relaxed));
            const uint64_t native_failures =
                hardware_decoder_failure_->native_allocation_failures.load(std::memory_order_relaxed) +
                hardware_decoder_failure_->native_signal_failures.load(std::memory_order_relaxed);
            const uint64_t ring_drops =
                hardware_decoder_failure_->native_ring_drops.load(std::memory_order_relaxed);
            if (native_failures != 0 || ring_drops != 0)
            {
                status += " fail=" + std::to_string(native_failures);
                status += " drop=" + std::to_string(ring_drops);
                status += " hr=" + std::to_string(
                    hardware_decoder_failure_->native_failure_hresult.load(std::memory_order_relaxed));
            }
            status += "]";
        }
#endif
        return status;
    }

    bool SendI420(uint32_t width, uint32_t height, const uint8_t* data, size_t size, int64_t timestamp_usec)
    {
        if (!server_ || !IsReady() || !IsConnected() || !video_source_ || data == nullptr || width == 0 || height == 0 ||
            width > kMaxVideoDimension || height > kMaxVideoDimension || (width & 1u) || (height & 1u))
            return false;
        const size_t y_size = static_cast<size_t>(width) * height;
        const size_t uv_size = static_cast<size_t>(width / 2u) * (height / 2u);
        if (size != y_size + uv_size * 2u)
            return false;
        auto buffer = webrtc::I420Buffer::Create(static_cast<int>(width), static_cast<int>(height));
        if (!buffer)
            return false;
        const uint8_t* source_y = data;
        const uint8_t* source_u = source_y + y_size;
        const uint8_t* source_v = source_u + uv_size;
        for (uint32_t row = 0; row < height; ++row)
            std::memcpy(buffer->MutableDataY() + row * buffer->StrideY(), source_y + row * width, width);
        for (uint32_t row = 0; row < height / 2u; ++row)
        {
            std::memcpy(buffer->MutableDataU() + row * buffer->StrideU(), source_u + row * (width / 2u), width / 2u);
            std::memcpy(buffer->MutableDataV() + row * buffer->StrideV(), source_v + row * (width / 2u), width / 2u);
        }
        webrtc::VideoFrame frame = webrtc::VideoFrame::Builder{}
            .set_video_frame_buffer(buffer)
            .set_timestamp_us(timestamp_usec)
            .build();
        video_source_->Push(frame);
        sent_frames_.fetch_add(1);
        return true;
    }

    bool SendI420Planes(
        uint32_t width,
        uint32_t height,
        const uint8_t* y_plane,
        uint32_t y_stride,
        const uint8_t* u_plane,
        uint32_t u_stride,
        const uint8_t* v_plane,
        uint32_t v_stride,
        int64_t timestamp_usec,
        std::function<void()> no_longer_used)
    {
        auto release_state = std::make_shared<std::function<void()>>(std::move(no_longer_used));
        auto release_once = [release_state]() {
            if (*release_state)
            {
                auto callback = std::move(*release_state);
                callback();
            }
        };
        if (!server_ || !IsReady() || !IsConnected() || !video_source_ ||
            y_plane == nullptr || u_plane == nullptr || v_plane == nullptr ||
            width == 0 || height == 0 || width > kMaxVideoDimension || height > kMaxVideoDimension ||
            (width & 1u) || (height & 1u) || y_stride < width ||
            u_stride < width / 2u || v_stride < width / 2u)
        {
            release_once();
            return false;
        }
        auto buffer = webrtc::make_ref_counted<RetainedI420Buffer>(
            static_cast<int>(width),
            static_cast<int>(height),
            y_plane,
            static_cast<int>(y_stride),
            u_plane,
            static_cast<int>(u_stride),
            v_plane,
            static_cast<int>(v_stride),
            release_once);
        if (!buffer)
        {
            release_once();
            return false;
        }
        webrtc::VideoFrame frame = webrtc::VideoFrame::Builder{}
            .set_video_frame_buffer(buffer)
            .set_timestamp_us(timestamp_usec)
            .build();
        video_source_->Push(frame);
        sent_frames_.fetch_add(1);
        return true;
    }

#if defined(_WIN32)
    bool SendNV12Surface(
        const NPWindowsNativeNV12Surface& surface,
        uint32_t rtp_timestamp,
        int64_t timestamp_usec,
        std::function<void()> release,
        std::function<void()> completion_scheduled)
    {
        if (!server_ || !native_nv12_enabled_ || !IsReady() || !IsConnected() ||
            !video_source_ || surface.width == 0 || surface.height == 0 ||
            surface.width > kMaxVideoDimension ||
            surface.height > kMaxVideoDimension)
        {
            if (release)
                release();
            return false;
        }
        auto buffer = np_create_windows_native_nv12_buffer(
            surface, std::move(release), std::move(completion_scheduled));
        if (!buffer)
            return false;
        const webrtc::VideoFrame frame = webrtc::VideoFrame::Builder{}
            .set_video_frame_buffer(buffer)
            .set_rtp_timestamp(rtp_timestamp)
            .set_timestamp_us(timestamp_usec)
            .build();
        video_source_->Push(frame);
        sent_frames_.fetch_add(1);
        return true;
    }
#endif

    bool SupportsNativeNV12() const
    {
        if (!server_ || !native_nv12_enabled_ || !IsReady())
            return false;
#if defined(_WIN32)
        if (hardware_encoder_failure_ &&
            (hardware_encoder_failure_->failed.load(std::memory_order_acquire) ||
                hardware_encoder_failure_->native_surface_failed.load(
                    std::memory_order_acquire)))
            return false;
#endif
        std::lock_guard lock(codec_telemetry_->text_mutex);
        std::string negotiated_codec = codec_telemetry_->codec_name;
        std::transform(negotiated_codec.begin(), negotiated_codec.end(),
            negotiated_codec.begin(), [](unsigned char value) {
                return static_cast<char>(std::tolower(value));
            });
        // A VP8 encoder cannot consume our native DX12 handle. Keep sending
        // retained I420 until RTC stats confirm that H264 is the negotiated
        // codec, and return to I420 immediately after a negotiation fallback.
        return negotiated_codec.find("h264") != std::string::npos;
    }

    void ReportNativeNV12Failure()
    {
#if defined(_WIN32)
        if (!server_ && hardware_decoder_failure_)
        {
            // Importing or synchronizing the retained decoder surface failed
            // in the Wicked DX12 render path. Keep the Media Foundation H264
            // decoder alive, but make subsequent decoded frames use its I420
            // copy path instead of repeatedly returning unusable surfaces.
            hardware_decoder_failure_->native_surface_failed.store(
                true, std::memory_order_release);
        }
#endif
    }

    bool ReceiveI420(ReceivedVideoFrame& output)
    {
        if (server_)
            return false;
        std::lock_guard<std::mutex> lock(video_mutex_);
        if (received_video_queue_.empty())
            return false;
        output = std::move(received_video_queue_.front());
        received_video_queue_.pop_front();
        return true;
    }

    bool SendControl(const uint8_t* data, size_t size)
    {
        if (server_ || data == nullptr || size == 0 || size > kMaxControlBytes)
            return false;
        webrtc::scoped_refptr<webrtc::DataChannelInterface> channel;
        {
            std::lock_guard<std::mutex> lock(channel_mutex_);
            channel = control_channel_;
        }
        if (!channel || channel->state() != webrtc::DataChannelInterface::kOpen ||
            channel->buffered_amount() > kMaxDataChannelBufferedBytes || !signaling_thread_)
            return false;
        webrtc::CopyOnWriteBuffer payload{data, size};
        const bool sent = signaling_thread_->BlockingCall([channel, payload = std::move(payload)]() mutable {
            return channel->state() == webrtc::DataChannelInterface::kOpen && channel->Send(webrtc::DataBuffer{payload, true});
        });
        if (sent)
            sent_controls_.fetch_add(1);
        return sent;
    }

    bool ReceiveControl(std::vector<uint8_t>& output)
    {
        if (!server_)
            return false;
        std::lock_guard<std::mutex> lock(control_mutex_);
        if (latest_control_.empty())
            return false;
        output = std::move(latest_control_);
        latest_control_.clear();
        return true;
    }

    bool SendFrameMetadata(const uint8_t* data, size_t size)
    {
        if (!server_ || data == nullptr || size == 0 || size > kMaxControlBytes)
            return false;
        webrtc::scoped_refptr<webrtc::DataChannelInterface> channel;
        {
            std::lock_guard<std::mutex> lock(channel_mutex_);
            channel = frame_metadata_channel_;
        }
        if (!channel || channel->state() != webrtc::DataChannelInterface::kOpen ||
            channel->buffered_amount() > kMaxDataChannelBufferedBytes || !signaling_thread_)
            return false;
        webrtc::CopyOnWriteBuffer payload{data, size};
        return signaling_thread_->BlockingCall([channel, payload = std::move(payload)]() mutable {
            return channel->state() == webrtc::DataChannelInterface::kOpen &&
                channel->Send(webrtc::DataBuffer{payload, true});
        });
    }

    bool ReceiveFrameMetadata(std::vector<uint8_t>& output)
    {
        if (server_)
            return false;
        std::lock_guard<std::mutex> lock(frame_metadata_mutex_);
        if (received_frame_metadata_queue_.empty())
            return false;
        output = std::move(received_frame_metadata_queue_.front());
        received_frame_metadata_queue_.pop_front();
        return true;
    }

    bool RequestKeyframe()
    {
        if (!server_ || !signaling_thread_ || !local_video_sender_)
            return false;
        const webrtc::scoped_refptr<webrtc::RtpSenderInterface> sender = local_video_sender_;
        return signaling_thread_->BlockingCall([sender]() {
            return sender->GenerateKeyFrame({}).ok();
        });
    }

    uint64_t SentFrames() const { return sent_frames_.load(); }
    uint64_t ReceivedFrames() const { return received_frames_.load(); }
    uint64_t DroppedFrames() const { return dropped_frames_.load(); }
    uint32_t DecodedQueueDepth()
    {
        std::lock_guard<std::mutex> lock(video_mutex_);
        return static_cast<uint32_t>(received_video_queue_.size());
    }
    uint64_t SentControls() const { return sent_controls_.load(); }
    uint64_t ReceivedControls() const { return received_controls_.load(); }

    void ReadCodecTelemetry(NPWebRTCBridgeStats& stats)
    {
        MaybeRequestCodecTelemetry();
#if defined(__APPLE__)
        if (server_ && hardware_encoder_failure_ &&
            hardware_encoder_failure_->failed.load(std::memory_order_acquire))
        {
            active_encoder_mode_ = "fallback-software";
            fallback_reason_ = "hardware-runtime-failed";
        }
        if (!server_ && hardware_decoder_failure_ &&
            hardware_decoder_failure_->failed.load(std::memory_order_acquire))
        {
            input_surface_ = "i420-decoded";
            fallback_reason_ = "hardware-decoder-runtime-failed";
        }
#elif defined(_WIN32)
        if (server_ && hardware_encoder_failure_ &&
            hardware_encoder_failure_->failed.load(std::memory_order_acquire))
        {
            active_encoder_mode_ = "fallback-software";
            fallback_reason_ = "hardware-runtime-failed";
            input_surface_ = "i420-readback";
        }
        if (server_ && hardware_encoder_failure_ &&
            hardware_encoder_failure_->native_surface_failed.load(
                std::memory_order_acquire))
        {
            native_nv12_enabled_ = false;
            input_surface_ = "i420-readback";
            fallback_reason_ = "native-surface-failed-i420";
        }
        if (!server_ && hardware_decoder_failure_ &&
            hardware_decoder_failure_->failed.load(std::memory_order_acquire))
        {
            native_nv12_enabled_ = false;
            input_surface_ = "i420-decoded";
        }
        if (!server_ && hardware_decoder_failure_ &&
            hardware_decoder_failure_->native_surface_failed.load(
                std::memory_order_acquire))
        {
            native_nv12_enabled_ = false;
            input_surface_ = "i420-decoded";
            fallback_reason_ = "native-surface-failed-i420";
        }
#endif
        stats.compressed_bytes_sent = codec_telemetry_->compressed_bytes_sent.load(std::memory_order_relaxed);
        stats.compressed_bytes_received = codec_telemetry_->compressed_bytes_received.load(std::memory_order_relaxed);
        stats.total_encode_time_usec = codec_telemetry_->total_encode_time_usec.load(std::memory_order_relaxed);
        stats.total_decode_time_usec = codec_telemetry_->total_decode_time_usec.load(std::memory_order_relaxed);
        stats.frames_encoded = codec_telemetry_->frames_encoded.load(std::memory_order_relaxed);
        stats.frames_decoded = codec_telemetry_->frames_decoded.load(std::memory_order_relaxed);
        stats.power_efficient_codec = codec_telemetry_->power_efficient.load(std::memory_order_relaxed) ? 1u : 0u;
        std::lock_guard lock(codec_telemetry_->text_mutex);
        if (server_ && codec_telemetry_->codec_name != "unknown")
        {
            std::string negotiated_codec = codec_telemetry_->codec_name;
            std::transform(negotiated_codec.begin(), negotiated_codec.end(),
                negotiated_codec.begin(), [](unsigned char value) {
                    return static_cast<char>(std::tolower(value));
                });
            if (negotiated_codec.find("vp8") != std::string::npos)
            {
                active_encoder_mode_ = encoder_preference_ == NP_WEBRTC_ENCODER_SOFTWARE
                    ? "software" : "fallback-software";
                input_surface_ = "i420-readback";
                if (encoder_preference_ == NP_WEBRTC_ENCODER_HARDWARE &&
                    fallback_reason_ == "none")
                    fallback_reason_ = "peer-negotiated-software";
            }
            else if (negotiated_codec.find("h264") != std::string::npos &&
                encoder_preference_ == NP_WEBRTC_ENCODER_HARDWARE
#if defined(__APPLE__)
                && !(hardware_encoder_failure_ &&
                    hardware_encoder_failure_->failed.load(
                        std::memory_order_acquire))
#elif defined(_WIN32)
                && !(hardware_encoder_failure_ &&
                    hardware_encoder_failure_->failed.load(
                        std::memory_order_acquire))
#endif
                )
            {
                active_encoder_mode_ = "hardware";
                fallback_reason_ = "none";
                input_surface_ = native_nv12_enabled_
                    ? "dx12-nv12-retained" : "i420-readback";
            }
        }
        else if (!server_ && codec_telemetry_->codec_name != "unknown")
        {
            std::string negotiated_codec = codec_telemetry_->codec_name;
            std::transform(negotiated_codec.begin(), negotiated_codec.end(),
                negotiated_codec.begin(), [](unsigned char value) {
                    return static_cast<char>(std::tolower(value));
                });
            input_surface_ = native_nv12_enabled_ &&
                negotiated_codec.find("h264") != std::string::npos
                ? "d3d11-nv12-retained" : "i420-decoded";
        }
#if defined(_WIN32)
        if (server_ && hardware_encoder_failure_ &&
            hardware_encoder_failure_->native_surface_failed.load(
                std::memory_order_acquire))
        {
            // The H264 MFT remains active; only zero-copy input fell back.
            active_encoder_mode_ = "hardware";
            input_surface_ = "i420-readback";
            fallback_reason_ = "native-surface-failed-i420";
        }
#endif
        const size_t codec_count = std::min(codec_telemetry_->codec_name.size(), sizeof(stats.codec_name) - 1u);
        std::memcpy(stats.codec_name, codec_telemetry_->codec_name.data(), codec_count);
        const size_t implementation_count =
            std::min(codec_telemetry_->implementation.size(), sizeof(stats.codec_implementation) - 1u);
        std::memcpy(stats.codec_implementation, codec_telemetry_->implementation.data(), implementation_count);
        const auto copy_text = [](char* destination, size_t capacity, const std::string& source) {
            const size_t count = std::min(source.size(), capacity - 1u);
            std::memcpy(destination, source.data(), count);
        };
        copy_text(stats.requested_encoder_mode, sizeof(stats.requested_encoder_mode), requested_encoder_mode_);
        copy_text(stats.active_encoder_mode, sizeof(stats.active_encoder_mode), active_encoder_mode_);
        copy_text(stats.input_surface, sizeof(stats.input_surface), input_surface_);
        copy_text(stats.codec_profile, sizeof(stats.codec_profile),
            codec_telemetry_->codec_profile == "unknown"
                ? codec_profile_ : codec_telemetry_->codec_profile);
        copy_text(stats.fallback_reason, sizeof(stats.fallback_reason), fallback_reason_);
    }

private:
    void MaybeRequestCodecTelemetry()
    {
        if (!peer_connection_ || !stats_callback_)
            return;
        const uint64_t now = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        uint64_t previous = last_stats_request_usec_.load(std::memory_order_relaxed);
        if (now < previous + 1'000'000ull ||
            !last_stats_request_usec_.compare_exchange_strong(previous, now, std::memory_order_relaxed))
            return;
        peer_connection_->GetStats(stats_callback_.get());
    }

    class VideoSource : public webrtc::AdaptedVideoTrackSource
    {
    public:
        SourceState state() const override { return SourceState::kLive; }
        bool remote() const override { return false; }
        bool is_screencast() const override { return true; }
        std::optional<bool> needs_denoising() const override { return false; }
        void Push(const webrtc::VideoFrame& frame) { OnFrame(frame); }
    };

    class VideoSink final : public webrtc::VideoSinkInterface<webrtc::VideoFrame>
    {
    public:
        explicit VideoSink(WebRTCSession& owner) : owner_(&owner) {}
        void OnFrame(const webrtc::VideoFrame& frame) override
        {
            if (owner_)
                owner_->OnVideoFrame(frame);
        }
    private:
        WebRTCSession* owner_ = nullptr;
    };

    class ChannelObserver final : public webrtc::DataChannelObserver
    {
    public:
        ChannelObserver(WebRTCSession& owner, bool frame_metadata) : owner_(&owner), frame_metadata_(frame_metadata) {}
        void OnStateChange() override { if (owner_) owner_->UpdateChannelStatus(); }
        void OnMessage(const webrtc::DataBuffer& buffer) override
        {
            if (!owner_)
                return;
            if (frame_metadata_)
                owner_->OnFrameMetadataMessage(buffer);
            else
                owner_->OnControlMessage(buffer);
        }
        void OnBufferedAmountChange(uint64_t) override {}
    private:
        WebRTCSession* owner_ = nullptr;
        bool frame_metadata_ = false;
    };

    class CreateDescriptionObserver : public webrtc::CreateSessionDescriptionObserver
    {
    public:
        CreateDescriptionObserver(WebRTCSession& owner, bool offer) : owner_(&owner), offer_(offer) {}
        void OnSuccess(webrtc::SessionDescriptionInterface* description) override
        {
            if (!owner_ || !description)
                return;
            owner_->pending_create_observer_ = nullptr;
            std::unique_ptr<webrtc::SessionDescriptionInterface> owned{description};
            std::string sdp;
            owned->ToString(&sdp);
            owner_->SetLocalDescription(std::move(owned));
            owner_->SendSignal(offer_ ? "offer" : "answer", Base64Encode(sdp));
            if (offer_)
                owner_->offer_in_flight_ = false;
        }
        void OnFailure(webrtc::RTCError error) override
        {
            if (owner_)
            {
                owner_->SetStatus(std::string{"SDP creation failed: "} + error.message());
                owner_->offer_in_flight_ = false;
                owner_->pending_create_observer_ = nullptr;
            }
        }
    private:
        WebRTCSession* owner_ = nullptr;
        bool offer_ = false;
    };

    class SetLocalObserver : public webrtc::SetLocalDescriptionObserverInterface
    {
    public:
        explicit SetLocalObserver(WebRTCSession& owner) : owner_(&owner) {}
        void OnSetLocalDescriptionComplete(webrtc::RTCError error) override
        {
            if (owner_ && error.ok())
                owner_->has_local_description_ = true;
            else if (owner_)
                owner_->SetStatus(std::string{"Set local SDP failed: "} + error.message());
        }
    private:
        WebRTCSession* owner_ = nullptr;
    };

    class SetRemoteObserver : public webrtc::SetRemoteDescriptionObserverInterface
    {
    public:
        SetRemoteObserver(WebRTCSession& owner, bool answer) : owner_(&owner), answer_(answer) {}
        void OnSetRemoteDescriptionComplete(webrtc::RTCError error) override
        {
            if (!owner_)
                return;
            if (!error.ok())
            {
                owner_->SetStatus(std::string{"Set remote SDP failed: "} + error.message());
                return;
            }
            owner_->has_remote_description_ = true;
            owner_->ApplyPendingIce();
            if (answer_)
                owner_->CreateAnswer();
        }
    private:
        WebRTCSession* owner_ = nullptr;
        bool answer_ = false;
    };

    struct PendingIce
    {
        std::string mid;
        int line = 0;
        std::string candidate;
    };

    void Initialize()
    {
        SetStatus("Starting native WebRTC");
        std::call_once(ssl_once_, [] { webrtc::InitializeSSL(); });
        webrtc::LogMessage::LogToDebug(webrtc::LS_WARNING);
        webrtc::LogMessage::SetLogToStderr(false);

        // Connect signaling before constructing PeerConnection. If the relay
        // is unavailable, the transport lifecycle will retry this session.
        // Constructing and immediately closing a PeerConnection here races
        // RtcEventLogImpl::StopLogging() on WebRTC's GCD task queue on macOS.
        if (!websocket_.Connect(signaling_url_))
            return Fail("Could not connect signaling WebSocket (only ws:// is supported)");

        network_thread_ = webrtc::Thread::CreateWithSocketServer().release();
        worker_thread_ = webrtc::Thread::Create().release();
        signaling_thread_ = webrtc::Thread::Create().release();
        if (!network_thread_)
            return Fail("Could not allocate WebRTC network thread");
        if (!worker_thread_)
            return Fail("Could not allocate WebRTC worker thread");
        if (!signaling_thread_)
            return Fail("Could not allocate WebRTC signaling thread");
        network_thread_started_ = network_thread_->Start();
        if (!network_thread_started_)
            return Fail("Could not start WebRTC network thread");
        worker_thread_started_ = worker_thread_->Start();
        if (!worker_thread_started_)
            return Fail("Could not start WebRTC worker thread");
        signaling_thread_started_ = signaling_thread_->Start();
        if (!signaling_thread_started_)
            return Fail("Could not start WebRTC signaling thread");
        task_queue_factory_ = webrtc::CreateDefaultTaskQueueFactory();
        if (!task_queue_factory_)
            return Fail("Could not create WebRTC task queue factory");
        stats_callback_ = webrtc::make_ref_counted<CodecStatsCallback>(codec_telemetry_);
        std::unique_ptr<webrtc::VideoEncoderFactory> encoder_factory;
        std::unique_ptr<webrtc::VideoDecoderFactory> decoder_factory;
#if defined(__APPLE__)
        NPAppleVideoCodecFactories apple_factories =
            np_create_apple_video_codec_factories(
                server_ && encoder_preference_ == NP_WEBRTC_ENCODER_HARDWARE);
        hardware_encoder_failure_ = apple_factories.hardware_failure;
        hardware_decoder_failure_ =
            apple_factories.hardware_decoder_failure;
        encoder_factory = std::move(apple_factories.encoder);
        decoder_factory = std::move(apple_factories.decoder);
        if (server_ && encoder_preference_ == NP_WEBRTC_ENCODER_HARDWARE)
        {
            if (apple_factories.hardware_encoder_available)
            {
                active_encoder_mode_ = "hardware";
                fallback_reason_ = "none";
            }
            else
            {
                active_encoder_mode_ = "fallback-software";
                fallback_reason_ = "hardware-capability-probe-failed";
            }
        }
#elif defined(_WIN32)
        NPWindowsVideoCodecFactories windows_factories =
            np_create_windows_video_codec_factories(
                server_ && encoder_preference_ == NP_WEBRTC_ENCODER_HARDWARE,
                adapter_luid_);
        hardware_encoder_failure_ = windows_factories.hardware_encoder_failure;
        hardware_decoder_failure_ = windows_factories.hardware_decoder_failure;
        encoder_factory = std::move(windows_factories.encoder);
        decoder_factory = std::move(windows_factories.decoder);
        if (server_ && encoder_preference_ == NP_WEBRTC_ENCODER_HARDWARE)
        {
            if (windows_factories.hardware_encoder_available)
            {
                // Capability enumeration is not proof that the negotiated
                // sender has produced hardware output. Promote this to
                // "hardware" only after RTC stats report active H264.
                active_encoder_mode_ = "hardware-probed";
                fallback_reason_ = "none";
                native_nv12_enabled_ = adapter_luid_ != 0;
                // The first frames stay on the retained I420 contract until
                // RTC stats confirm that this connection negotiated H264.
                input_surface_ = "i420-readback";
            }
            else
            {
                active_encoder_mode_ = "fallback-software";
                fallback_reason_ = "hardware-capability-probe-failed";
            }
        }
        else if (!server_ && windows_factories.hardware_decoder_available &&
            adapter_luid_ != 0)
        {
            native_nv12_enabled_ = true;
            input_surface_ = "i420-decoded";
        }
#else
        encoder_factory = std::make_unique<webrtc::VideoEncoderFactoryTemplate<
            webrtc::LibvpxVp8EncoderTemplateAdapter>>();
        decoder_factory = std::make_unique<webrtc::VideoDecoderFactoryTemplate<
            webrtc::LibvpxVp8DecoderTemplateAdapter>>();
#endif
        peer_factory_ = webrtc::CreatePeerConnectionFactory(
            network_thread_, worker_thread_, signaling_thread_, nullptr,
            webrtc::CreateBuiltinAudioEncoderFactory(), webrtc::CreateBuiltinAudioDecoderFactory(),
            std::move(encoder_factory), std::move(decoder_factory), nullptr, nullptr);
        if (!peer_factory_)
            return Fail("Could not create PeerConnectionFactory");
        webrtc::PeerConnectionInterface::RTCConfiguration configuration = {};
        configuration.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
        if (use_internet_ice_)
        {
            webrtc::PeerConnectionInterface::IceServer stun;
            stun.urls.push_back("stun:stun.l.google.com:19302");
            configuration.servers.push_back(std::move(stun));
        }
        webrtc::PeerConnectionDependencies dependencies{this};
        auto peer_or_error = peer_factory_->CreatePeerConnectionOrError(configuration, std::move(dependencies));
        if (!peer_or_error.ok())
            return Fail(std::string{"Could not create PeerConnection: "} + peer_or_error.error().message());
        peer_connection_ = peer_or_error.MoveValue();
        if (server_)
        {
            CreateControlChannel();
            CreateFrameMetadataChannel();
            CreateLocalVideoTrack();
            if (failed_.load(std::memory_order_acquire))
                return;
        }
        SendSignaling("join|" + room_id_ + "|" + Role());
        signaling_receive_thread_ = std::thread([this] { SignalingLoop(); });
        initialized_.store(true);
        SetStatus("Joined signaling room; waiting for peer");
    }

    void Fail(std::string message)
    {
        failed_.store(true);
        SetStatus(std::move(message));
    }

    void Shutdown()
    {
        if (stop_requested_.exchange(true))
            return;
        initialized_.store(false);
        peer_connected_.store(false);
        websocket_.Close();
        if (signaling_receive_thread_.joinable())
            signaling_receive_thread_.join();
        auto release_peer_state = [this] {
            if (control_channel_ && channel_observer_)
                control_channel_->UnregisterObserver();
            if (frame_metadata_channel_ && frame_metadata_channel_observer_)
                frame_metadata_channel_->UnregisterObserver();
            if (remote_video_track_ && video_sink_)
                remote_video_track_->RemoveSink(video_sink_.get());
            if (peer_connection_)
                peer_connection_->Close();
            control_channel_ = nullptr;
            channel_observer_.reset();
            frame_metadata_channel_ = nullptr;
            frame_metadata_channel_observer_.reset();
            remote_video_track_ = nullptr;
            video_sink_.reset();
            local_video_track_ = nullptr;
            local_video_sender_ = nullptr;
            local_video_transceiver_ = nullptr;
            video_source_ = nullptr;
            peer_connection_ = nullptr;
        };
        if (signaling_thread_ && signaling_thread_started_)
        {
            signaling_thread_->BlockingCall(release_peer_state);
        }
        else
            release_peer_state();
        pending_create_observer_ = nullptr;
        peer_factory_ = nullptr;
        task_queue_factory_.reset();
        if (network_thread_ && network_thread_started_)
            network_thread_->Stop();
        if (worker_thread_ && worker_thread_started_)
            worker_thread_->Stop();
        if (signaling_thread_ && signaling_thread_started_)
            signaling_thread_->Stop();
        delete network_thread_;
        delete worker_thread_;
        delete signaling_thread_;
        network_thread_ = nullptr;
        worker_thread_ = nullptr;
        signaling_thread_ = nullptr;
    }

    std::string Role() const { return server_ ? "server" : "client"; }

    void SetStatus(std::string value)
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        status_ = std::move(value);
    }

    void SendSignaling(const std::string& message)
    {
        std::lock_guard<std::mutex> lock(signaling_send_mutex_);
        websocket_.SendText(message);
    }

    void SendSignal(const std::string& type, const std::string& payload)
    {
        SendSignaling("signal|" + room_id_ + "|" + Role() + "|" + type + "|" + payload);
    }

    void SignalingLoop()
    {
        std::string message;
        while (!stop_requested_.load())
        {
            if (!websocket_.ReceiveText(message))
                break;
            if (!message.empty())
                HandleSignaling(message);
        }
        peer_connected_.store(false);
        if (!stop_requested_.load())
            Fail("Signaling WebSocket disconnected");
    }

    void HandleSignaling(const std::string& message)
    {
        if (signaling_thread_ && !signaling_thread_->IsCurrent())
        {
            signaling_thread_->BlockingCall([this, message] { HandleSignaling(message); });
            return;
        }
        const std::vector<std::string> tokens = SplitTokens(message, '|');
        if (tokens.empty())
            return;
        if (tokens[0] == "peer")
        {
            if (server_)
                CreateOffer();
            return;
        }
        if (tokens[0] == "error")
        {
            if (tokens.size() >= 3 && tokens[1] == "duplicate-role")
            {
                return Fail("Signaling rejected this " + Role() +
                    " because another process already owns room '" + room_id_ +
                    "'; close the older NewPipeline process or use a different room");
            }
            return Fail("Signaling rejected the session: " + message);
        }
        if (tokens[0] != "signal" || tokens.size() < 4)
            return;
        std::string from_role;
        std::string type;
        std::string payload;
        if (tokens.size() >= 5 && (tokens[2] == "client" || tokens[2] == "server"))
        {
            from_role = tokens[2];
            type = tokens[3];
            payload = JoinTokens(tokens, 4, '|');
        }
        else
        {
            from_role = tokens[1];
            type = tokens[2];
            payload = JoinTokens(tokens, 3, '|');
        }
        if (from_role == Role())
            return;
        if (type == "offer" || type == "answer")
        {
            const auto sdp = Base64Decode(payload);
            if (sdp)
                ApplyRemoteDescription(type == "offer" ? webrtc::SdpType::kOffer : webrtc::SdpType::kAnswer, *sdp);
        }
        else if (type == "ice")
        {
            const auto parts = SplitTokens(payload, '|');
            if (parts.size() >= 3)
            {
                const auto mid = Base64Decode(parts[0]);
                const auto candidate = Base64Decode(JoinTokens(parts, 2, '|'));
                const int line = std::atoi(parts[1].c_str());
                if (mid && candidate && line >= 0)
                    AddRemoteIce(*mid, line, *candidate);
            }
        }
    }

    void CreateControlChannel()
    {
        webrtc::DataChannelInit init = {};
        init.ordered = true;
        auto channel_or_error = peer_connection_->CreateDataChannelOrError("np.control", &init);
        if (channel_or_error.ok())
            AttachControlChannel(channel_or_error.MoveValue());
        else
            SetStatus(std::string{"Could not create control DataChannel: "} + channel_or_error.error().message());
    }

    void CreateFrameMetadataChannel()
    {
        webrtc::DataChannelInit init = {};
        init.ordered = false;
        init.maxRetransmits = 1;
        auto channel_or_error = peer_connection_->CreateDataChannelOrError("np.frame_meta", &init);
        if (channel_or_error.ok())
            AttachFrameMetadataChannel(channel_or_error.MoveValue());
        else
            SetStatus(std::string{"Could not create frame metadata DataChannel: "} +
                channel_or_error.error().message());
    }

    void CreateLocalVideoTrack()
    {
        video_source_ = webrtc::make_ref_counted<VideoSource>();
        local_video_track_ = peer_factory_->CreateVideoTrack(video_source_, "np.remote.video");
        if (!local_video_track_)
            return Fail("Could not create np.remote.video track");
        webrtc::RtpTransceiverInit init = {};
        init.direction = webrtc::RtpTransceiverDirection::kSendOnly;
        init.stream_ids.push_back("np.stream");
        webrtc::RtpEncodingParameters encoding = {};
        encoding.min_bitrate_bps = kBufferVideoMinBitrateBps;
        encoding.max_bitrate_bps = kBufferVideoMaxBitrateBps;
        encoding.max_framerate = kBufferVideoMaxFramerate;
        encoding.scale_resolution_down_by = 1.0;
        encoding.bitrate_priority = 4.0;
        encoding.network_priority = webrtc::Priority::kHigh;
        init.send_encodings.push_back(std::move(encoding));
        auto transceiver_or_error = peer_connection_->AddTransceiver(local_video_track_, init);
        if (!transceiver_or_error.ok())
            return Fail(std::string{"Could not add video transceiver: "} + transceiver_or_error.error().message());
        local_video_transceiver_ = transceiver_or_error.MoveValue();
#if defined(_WIN32)
        if (encoder_preference_ == NP_WEBRTC_ENCODER_HARDWARE &&
            (active_encoder_mode_ == "hardware-probed" ||
                active_encoder_mode_ == "hardware"))
        {
            // Factory order alone does not override WebRTC's default video
            // codec preference.  Put every advertised H264 profile before
            // VP8 on this transceiver before the offer is created, otherwise
            // two endpoints that support both codecs can still select VP8.
            auto preferences = peer_factory_
                ->GetRtpSenderCapabilities(webrtc::MediaType::VIDEO).codecs;
            const auto first_non_h264 = std::stable_partition(
                preferences.begin(), preferences.end(),
                [](const webrtc::RtpCodecCapability& codec) {
                    std::string name = codec.name;
                    std::transform(name.begin(), name.end(), name.begin(),
                        [](unsigned char value) {
                            return static_cast<char>(std::tolower(value));
                        });
                    return name == "h264";
                });
            if (first_non_h264 == preferences.begin())
            {
                if (hardware_encoder_failure_)
                    hardware_encoder_failure_->failed.store(
                        true, std::memory_order_release);
                return Fail("Media Foundation encoder was detected but WebRTC exposed no H264 sender capability");
            }
            const webrtc::RTCError result =
                local_video_transceiver_->SetCodecPreferences(preferences);
            if (!result.ok())
            {
                if (hardware_encoder_failure_)
                    hardware_encoder_failure_->failed.store(
                        true, std::memory_order_release);
                return Fail(std::string{"Could not prefer H264 for the hardware video transceiver: "} +
                    result.message());
            }
        }
#endif
        local_video_sender_ = local_video_transceiver_->sender();
        if (local_video_sender_)
        {
            webrtc::RtpParameters parameters = local_video_sender_->GetParameters();
            parameters.degradation_preference = webrtc::DegradationPreference::MAINTAIN_RESOLUTION;
            if (!parameters.encodings.empty())
            {
                parameters.encodings[0].min_bitrate_bps = kBufferVideoMinBitrateBps;
                parameters.encodings[0].max_bitrate_bps = kBufferVideoMaxBitrateBps;
                parameters.encodings[0].max_framerate = kBufferVideoMaxFramerate;
                parameters.encodings[0].scale_resolution_down_by = 1.0;
                parameters.encodings[0].bitrate_priority = 4.0;
                parameters.encodings[0].network_priority = webrtc::Priority::kHigh;
            }
            const webrtc::RTCError result = local_video_sender_->SetParameters(parameters);
            if (!result.ok())
                SetStatus(std::string{"Could not configure loss-sensitive video sender: "} + result.message());
        }
    }

    void AttachControlChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface> channel)
    {
        if (!channel || channel->label() != "np.control")
            return;
        {
            std::lock_guard<std::mutex> lock(channel_mutex_);
            if (control_channel_ && channel_observer_)
                control_channel_->UnregisterObserver();
            control_channel_ = std::move(channel);
            channel_observer_ = std::make_unique<ChannelObserver>(*this, false);
            control_channel_->RegisterObserver(channel_observer_.get());
        }
        UpdateChannelStatus();
    }


    void AttachFrameMetadataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface> channel)
    {
        if (!channel || channel->label() != "np.frame_meta")
            return;
        {
            std::lock_guard<std::mutex> lock(channel_mutex_);
            if (frame_metadata_channel_ && frame_metadata_channel_observer_)
                frame_metadata_channel_->UnregisterObserver();
            frame_metadata_channel_ = std::move(channel);
            frame_metadata_channel_observer_ = std::make_unique<ChannelObserver>(*this, true);
            frame_metadata_channel_->RegisterObserver(frame_metadata_channel_observer_.get());
        }
        UpdateChannelStatus();
    }

    void UpdateChannelStatus()
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        const bool control_open = control_channel_ &&
            control_channel_->state() == webrtc::DataChannelInterface::kOpen;
        const bool metadata_open = frame_metadata_channel_ &&
            frame_metadata_channel_->state() == webrtc::DataChannelInterface::kOpen;
        if (control_open || metadata_open)
        {
            std::string channels;
            if (control_open)
                channels = "control";
            if (metadata_open)
                channels += channels.empty() ? "frame metadata" : " + frame metadata";
            SetStatus(peer_connected_.load()
                ? "Video track connected; " + channels + " DataChannel open"
                : channels + " DataChannel open; ICE pending");
        }
    }


    void OnFrameMetadataMessage(const webrtc::DataBuffer& buffer)
    {
        if (server_ || !buffer.binary || buffer.size() == 0 || buffer.size() > kMaxControlBytes)
            return;
        std::lock_guard<std::mutex> lock(frame_metadata_mutex_);
        if (received_frame_metadata_queue_.size() >=
            kMaxReceiveMetadataQueueDepth)
            received_frame_metadata_queue_.pop_front();
        received_frame_metadata_queue_.emplace_back(
            buffer.data.data(), buffer.data.data() + buffer.size());
    }

    void OnControlMessage(const webrtc::DataBuffer& buffer)
    {
        if (!server_ || !buffer.binary || buffer.size() == 0 || buffer.size() > kMaxControlBytes)
            return;
        std::lock_guard<std::mutex> lock(control_mutex_);
        latest_control_.assign(buffer.data.data(), buffer.data.data() + buffer.size());
        received_controls_.fetch_add(1);
    }

    void AttachVideoTrack(webrtc::scoped_refptr<webrtc::VideoTrackInterface> track)
    {
        if (!track)
            return;
        if (remote_video_track_ && video_sink_)
            remote_video_track_->RemoveSink(video_sink_.get());
        remote_video_track_ = std::move(track);
        video_sink_ = std::make_unique<VideoSink>(*this);
        remote_video_track_->AddOrUpdateSink(video_sink_.get(), webrtc::VideoSinkWants{});
    }

    void OnVideoFrame(const webrtc::VideoFrame& frame)
    {
        const auto buffer = frame.video_frame_buffer();
        if (!buffer || buffer->width() <= 0 || buffer->height() <= 0 ||
            buffer->width() > static_cast<int>(kMaxVideoDimension) ||
            buffer->height() > static_cast<int>(kMaxVideoDimension))
            return;
        webrtc::scoped_refptr<webrtc::VideoFrameBuffer> retained = buffer;
#if defined(_WIN32)
        NPWindowsNativeNV12Surface native_surface;
        const bool native = np_get_windows_native_nv12_surface(
            buffer.get(), native_surface);
#else
        const bool native = false;
#endif
        if (!native)
        {
            const auto i420 = buffer->ToI420();
            if (!i420)
                return;
            retained = i420;
        }
        ReceivedVideoFrame received;
        received.width = static_cast<uint32_t>(retained->width());
        received.height = static_cast<uint32_t>(retained->height());
        received.rtp_timestamp = frame.rtp_timestamp();
#if defined(_WIN32)
        received.timestamp_usec = native &&
                native_surface.source_timestamp_usec > 0
            ? native_surface.source_timestamp_usec
            : frame.timestamp_us();
#else
        received.timestamp_usec = frame.timestamp_us();
#endif
        received.buffer = std::move(retained);
        {
            std::lock_guard<std::mutex> lock(video_mutex_);
            if (received_video_queue_.size() >= kMaxReceiveVideoQueueDepth)
            {
                received_video_queue_.pop_front();
                dropped_frames_.fetch_add(1);
            }
            received_video_queue_.push_back(std::move(received));
        }
        received_frames_.fetch_add(1);
    }

    void CreateOffer()
    {
        if (!peer_connection_ || offer_in_flight_)
            return;
        offer_in_flight_ = true;
        auto observer = webrtc::make_ref_counted<CreateDescriptionObserver>(*this, true);
        pending_create_observer_ = observer;
        peer_connection_->CreateOffer(observer.get(), webrtc::PeerConnectionInterface::RTCOfferAnswerOptions{});
    }

    void CreateAnswer()
    {
        auto observer = webrtc::make_ref_counted<CreateDescriptionObserver>(*this, false);
        pending_create_observer_ = observer;
        peer_connection_->CreateAnswer(observer.get(), webrtc::PeerConnectionInterface::RTCOfferAnswerOptions{});
    }

    void SetLocalDescription(std::unique_ptr<webrtc::SessionDescriptionInterface> description)
    {
        peer_connection_->SetLocalDescription(std::move(description), webrtc::make_ref_counted<SetLocalObserver>(*this));
    }

    void ApplyRemoteDescription(webrtc::SdpType type, const std::string& sdp)
    {
        webrtc::SdpParseError error;
        auto description = webrtc::CreateSessionDescription(type, sdp, &error);
        if (!description)
            return SetStatus("Could not parse remote SDP");
        peer_connection_->SetRemoteDescription(std::move(description),
            webrtc::make_ref_counted<SetRemoteObserver>(*this, type == webrtc::SdpType::kOffer));
    }

    void AddRemoteIce(const std::string& mid, int line, const std::string& candidate)
    {
        if (!has_remote_description_)
        {
            std::lock_guard<std::mutex> lock(ice_mutex_);
            pending_ice_.push_back({mid, line, candidate});
            return;
        }
        webrtc::SdpParseError error;
        std::unique_ptr<webrtc::IceCandidateInterface> ice{webrtc::CreateIceCandidate(mid, line, candidate, &error)};
        if (ice)
            peer_connection_->AddIceCandidate(ice.get());
    }

    void ApplyPendingIce()
    {
        std::vector<PendingIce> pending;
        {
            std::lock_guard<std::mutex> lock(ice_mutex_);
            pending.swap(pending_ice_);
        }
        for (const PendingIce& ice : pending)
            AddRemoteIce(ice.mid, ice.line, ice.candidate);
    }

    void OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState) override {}
    void OnAddStream(webrtc::scoped_refptr<webrtc::MediaStreamInterface>) override {}
    void OnRemoveStream(webrtc::scoped_refptr<webrtc::MediaStreamInterface>) override {}
    void OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface> channel) override
    {
        if (channel && channel->label() == "np.frame_meta")
            AttachFrameMetadataChannel(std::move(channel));
        else
            AttachControlChannel(std::move(channel));
    }
    void OnRenegotiationNeeded() override {}
    void OnIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState state) override
    {
        const bool connected = state == webrtc::PeerConnectionInterface::kIceConnectionConnected ||
            state == webrtc::PeerConnectionInterface::kIceConnectionCompleted;
        peer_connected_.store(connected);
        if (connected)
            SetStatus("WebRTC video track connected");
        else if (state == webrtc::PeerConnectionInterface::kIceConnectionFailed)
            SetStatus("WebRTC ICE failed");
    }
    void OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState) override {}
    void OnIceCandidate(const webrtc::IceCandidateInterface* candidate) override
    {
        if (!candidate)
            return;
        std::string text;
        if (candidate->ToString(&text))
            SendSignal("ice", Base64Encode(candidate->sdp_mid()) + "|" +
                std::to_string(candidate->sdp_mline_index()) + "|" + Base64Encode(text));
    }
    void OnAddTrack(webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver,
        const std::vector<webrtc::scoped_refptr<webrtc::MediaStreamInterface>>&) override
    {
        if (!receiver || !receiver->track() || receiver->track()->kind() != webrtc::MediaStreamTrackInterface::kVideoKind)
            return;
        AttachVideoTrack(webrtc::scoped_refptr<webrtc::VideoTrackInterface>(
            static_cast<webrtc::VideoTrackInterface*>(receiver->track().get())));
    }
    void OnTrack(webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) override
    {
        if (!transceiver || !transceiver->receiver())
            return;
        const auto track = transceiver->receiver()->track();
        if (track && track->kind() == webrtc::MediaStreamTrackInterface::kVideoKind)
            AttachVideoTrack(webrtc::scoped_refptr<webrtc::VideoTrackInterface>(static_cast<webrtc::VideoTrackInterface*>(track.get())));
    }

    bool server_ = false;
    std::string signaling_url_;
    std::string room_id_;
    bool use_internet_ice_ = false;
    NPWebRTCEncoderPreference encoder_preference_ = NP_WEBRTC_ENCODER_HARDWARE;
    uint64_t adapter_luid_ = 0;
    bool native_nv12_enabled_ = false;
    std::string requested_encoder_mode_ = "unknown";
    std::string active_encoder_mode_ = "unknown";
    std::string input_surface_ = "i420-readback";
    std::string codec_profile_ = "unknown";
    std::string fallback_reason_ = "none";
#if defined(__APPLE__)
    std::shared_ptr<NPHardwareEncoderFailureSignal> hardware_encoder_failure_;
    std::shared_ptr<NPHardwareDecoderFailureSignal> hardware_decoder_failure_;
#elif defined(_WIN32)
    std::shared_ptr<NPWindowsHardwareEncoderFailureSignal> hardware_encoder_failure_;
    std::shared_ptr<NPWindowsHardwareDecoderFailureSignal> hardware_decoder_failure_;
#endif
    std::atomic_bool initialized_{false};
    std::atomic_bool stop_requested_{false};
    std::atomic_bool peer_connected_{false};
    std::atomic_bool failed_{false};
    bool offer_in_flight_ = false;
    bool has_local_description_ = false;
    bool has_remote_description_ = false;
    WebSocketSignalingClient websocket_;
    std::thread signaling_receive_thread_;
    std::mutex signaling_send_mutex_;
    webrtc::Thread* network_thread_ = nullptr;
    webrtc::Thread* worker_thread_ = nullptr;
    webrtc::Thread* signaling_thread_ = nullptr;
    bool network_thread_started_ = false;
    bool worker_thread_started_ = false;
    bool signaling_thread_started_ = false;
    std::unique_ptr<webrtc::TaskQueueFactory> task_queue_factory_;
    std::shared_ptr<CodecTelemetry> codec_telemetry_ = std::make_shared<CodecTelemetry>();
    webrtc::scoped_refptr<CodecStatsCallback> stats_callback_;
    std::atomic<uint64_t> last_stats_request_usec_{0};
    webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> peer_factory_;
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> peer_connection_;
    webrtc::scoped_refptr<CreateDescriptionObserver> pending_create_observer_;
    webrtc::scoped_refptr<VideoSource> video_source_;
    webrtc::scoped_refptr<webrtc::VideoTrackInterface> local_video_track_;
    webrtc::scoped_refptr<webrtc::RtpSenderInterface> local_video_sender_;
    webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> local_video_transceiver_;
    webrtc::scoped_refptr<webrtc::VideoTrackInterface> remote_video_track_;
    std::unique_ptr<VideoSink> video_sink_;
    std::mutex video_mutex_;
    std::deque<ReceivedVideoFrame> received_video_queue_;
    std::mutex channel_mutex_;
    webrtc::scoped_refptr<webrtc::DataChannelInterface> control_channel_;
    std::unique_ptr<ChannelObserver> channel_observer_;
    webrtc::scoped_refptr<webrtc::DataChannelInterface> frame_metadata_channel_;
    std::unique_ptr<ChannelObserver> frame_metadata_channel_observer_;
    std::mutex control_mutex_;
    std::vector<uint8_t> latest_control_;
    std::mutex frame_metadata_mutex_;
    std::deque<std::vector<uint8_t>> received_frame_metadata_queue_;
    std::mutex ice_mutex_;
    std::vector<PendingIce> pending_ice_;
    mutable std::mutex status_mutex_;
    std::string status_;
    std::atomic<uint64_t> sent_frames_{0};
    std::atomic<uint64_t> received_frames_{0};
    std::atomic<uint64_t> dropped_frames_{0};
    std::atomic<uint64_t> sent_controls_{0};
    std::atomic<uint64_t> received_controls_{0};
    static std::once_flag ssl_once_;
};

std::once_flag WebRTCSession::ssl_once_;
}

struct NPWebRTCBridge
{
    std::unique_ptr<WebRTCSession> session;
    std::optional<ReceivedVideoFrame> pending_video;
    std::vector<uint8_t> pending_control;
    std::vector<uint8_t> pending_frame_metadata;
};

struct NPWebRTCVideoFrame
{
    ReceivedVideoFrame frame;
};

extern "C" NPWebRTCBridge* np_webrtc_bridge_create(
    int is_server, const char* signaling_url, const char* room_id,
    int use_internet_ice, int encoder_preference, uint64_t adapter_luid)
{
    if (!signaling_url || !room_id || signaling_url[0] == '\0' || room_id[0] == '\0')
        return nullptr;
    auto bridge = std::make_unique<NPWebRTCBridge>();
    const NPWebRTCEncoderPreference preference =
        encoder_preference == NP_WEBRTC_ENCODER_SOFTWARE
        ? NP_WEBRTC_ENCODER_SOFTWARE : NP_WEBRTC_ENCODER_HARDWARE;
    bridge->session = std::make_unique<WebRTCSession>(
        is_server != 0, signaling_url, room_id, use_internet_ice != 0,
        preference, adapter_luid);
    return bridge.release();
}

extern "C" void np_webrtc_bridge_destroy(NPWebRTCBridge* bridge)
{
    delete bridge;
}

extern "C" int np_webrtc_bridge_send_i420(
    NPWebRTCBridge* bridge, uint32_t width, uint32_t height, const uint8_t* data, size_t data_size, int64_t timestamp_usec)
{
    return bridge && bridge->session && bridge->session->SendI420(width, height, data, data_size, timestamp_usec) ? 1 : 0;
}

extern "C" int np_webrtc_bridge_send_i420_planes(
    NPWebRTCBridge* bridge,
    uint32_t width,
    uint32_t height,
    const uint8_t* y_plane,
    uint32_t y_stride,
    const uint8_t* u_plane,
    uint32_t u_stride,
    const uint8_t* v_plane,
    uint32_t v_stride,
    int64_t timestamp_usec,
    NPWebRTCReleaseCallback release_callback,
    void* release_context)
{
    auto release = [release_callback, release_context]() {
        if (release_callback)
            release_callback(release_context);
    };
    if (!bridge || !bridge->session)
    {
        release();
        return 0;
    }
    return bridge->session->SendI420Planes(
        width, height, y_plane, y_stride, u_plane, u_stride, v_plane, v_stride,
        timestamp_usec, std::move(release)) ? 1 : 0;
}

extern "C" int np_webrtc_bridge_send_nv12_surface(
    NPWebRTCBridge* bridge,
    uint32_t width,
    uint32_t height,
    void* texture_shared_handle,
    void* fence_shared_handle,
    uint64_t producer_fence_value,
    uint64_t consumer_fence_value,
    uint64_t adapter_luid,
    uint32_t rtp_timestamp,
    int64_t timestamp_usec,
    NPWebRTCReleaseCallback release_callback,
    void* release_context,
    NPWebRTCReleaseCallback completion_scheduled_callback,
    void* completion_scheduled_context)
{
    auto release = [release_callback, release_context]() {
        if (release_callback)
            release_callback(release_context);
    };
#if defined(_WIN32)
    if (!bridge || !bridge->session)
    {
        release();
        return 0;
    }
    NPWindowsNativeNV12Surface surface;
    surface.width = width;
    surface.height = height;
    surface.texture_shared_handle = texture_shared_handle;
    surface.fence_shared_handle = fence_shared_handle;
    surface.producer_fence_value = producer_fence_value;
    surface.consumer_fence_value = consumer_fence_value;
    surface.adapter_luid = adapter_luid;
    surface.source_timestamp_usec = timestamp_usec;
    auto completion = [completion_scheduled_callback,
                          completion_scheduled_context]() {
        if (completion_scheduled_callback)
            completion_scheduled_callback(completion_scheduled_context);
    };
    return bridge->session->SendNV12Surface(
        surface, rtp_timestamp, timestamp_usec,
        std::move(release), std::move(completion)) ? 1 : 0;
#else
    (void)bridge; (void)width; (void)height; (void)texture_shared_handle;
    (void)fence_shared_handle; (void)producer_fence_value;
    (void)consumer_fence_value; (void)adapter_luid; (void)rtp_timestamp;
    (void)timestamp_usec; (void)completion_scheduled_callback;
    (void)completion_scheduled_context;
    release();
    return 0;
#endif
}

extern "C" int np_webrtc_bridge_receive_i420(
    NPWebRTCBridge* bridge, uint32_t* width, uint32_t* height, uint8_t* destination,
    size_t destination_capacity, size_t* required_size)
{
    if (!bridge || !bridge->session || !width || !height || !required_size)
        return 0;
    if (!bridge->pending_video.has_value())
    {
        ReceivedVideoFrame frame;
        if (!bridge->session->ReceiveI420(frame))
            return 0;
        bridge->pending_video = std::move(frame);
    }
    *width = bridge->pending_video->width;
    *height = bridge->pending_video->height;
    const ReceivedVideoFrame& frame = *bridge->pending_video;
    const auto i420 = frame.buffer ? frame.buffer->ToI420() : nullptr;
    if (!i420)
    {
#if defined(_WIN32)
        bridge->session->ReportNativeNV12Failure();
#endif
        bridge->pending_video.reset();
        return 0;
    }
    const size_t y_size = static_cast<size_t>(frame.width) * frame.height;
    const size_t uv_size = static_cast<size_t>(frame.width / 2u) * (frame.height / 2u);
    *required_size = y_size + uv_size * 2u;
    if (!destination || destination_capacity < *required_size)
        return -1;
    uint8_t* destination_y = destination;
    uint8_t* destination_u = destination_y + y_size;
    uint8_t* destination_v = destination_u + uv_size;
    for (uint32_t row = 0; row < frame.height; ++row)
        std::memcpy(destination_y + static_cast<size_t>(row) * frame.width,
            i420->DataY() + static_cast<size_t>(row) * i420->StrideY(), frame.width);
    for (uint32_t row = 0; row < frame.height / 2u; ++row)
    {
        std::memcpy(destination_u + static_cast<size_t>(row) * (frame.width / 2u),
            i420->DataU() + static_cast<size_t>(row) * i420->StrideU(), frame.width / 2u);
        std::memcpy(destination_v + static_cast<size_t>(row) * (frame.width / 2u),
            i420->DataV() + static_cast<size_t>(row) * i420->StrideV(), frame.width / 2u);
    }
    bridge->pending_video.reset();
    return 1;
}

extern "C" int np_webrtc_bridge_acquire_i420_frame(
    NPWebRTCBridge* bridge, NPWebRTCVideoFrame** frame)
{
    if (np_webrtc_bridge_acquire_video_frame(bridge, frame) != 1 ||
        frame == nullptr || *frame == nullptr)
        return 0;
    if ((*frame)->frame.buffer == nullptr ||
        (*frame)->frame.buffer->GetI420() == nullptr)
    {
#if defined(_WIN32)
        if (bridge && bridge->session)
            bridge->session->ReportNativeNV12Failure();
#endif
        np_webrtc_video_frame_release(*frame);
        *frame = nullptr;
        return 0;
    }
    return 1;
}

extern "C" int np_webrtc_bridge_acquire_video_frame(
    NPWebRTCBridge* bridge, NPWebRTCVideoFrame** frame)
{
    if (!frame)
        return 0;
    *frame = nullptr;
    if (!bridge || !bridge->session)
        return 0;
    ReceivedVideoFrame received;
    if (!bridge->session->ReceiveI420(received) || !received.buffer)
        return 0;
    auto retained = std::make_unique<NPWebRTCVideoFrame>();
    retained->frame = std::move(received);
    *frame = retained.release();
    return 1;
}

extern "C" int np_webrtc_video_frame_get_i420(
    NPWebRTCVideoFrame* retained,
    uint32_t* width,
    uint32_t* height,
    const uint8_t** y_plane,
    uint32_t* y_stride,
    const uint8_t** u_plane,
    uint32_t* u_stride,
    const uint8_t** v_plane,
    uint32_t* v_stride,
    int64_t* timestamp_usec)
{
    if (!retained || !retained->frame.buffer || !width || !height ||
        !y_plane || !y_stride || !u_plane || !u_stride || !v_plane || !v_stride)
        return 0;
    const auto* i420 = retained->frame.buffer->GetI420();
    if (i420 == nullptr)
        return 0;
    if (i420->StrideY() <= 0 || i420->StrideU() <= 0 || i420->StrideV() <= 0)
        return 0;
    *width = retained->frame.width;
    *height = retained->frame.height;
    *y_plane = i420->DataY();
    *y_stride = static_cast<uint32_t>(i420->StrideY());
    *u_plane = i420->DataU();
    *u_stride = static_cast<uint32_t>(i420->StrideU());
    *v_plane = i420->DataV();
    *v_stride = static_cast<uint32_t>(i420->StrideV());
    if (timestamp_usec)
        *timestamp_usec = retained->frame.timestamp_usec;
    return 1;
}

extern "C" int np_webrtc_video_frame_get_nv12_surface(
    NPWebRTCVideoFrame* retained,
    uint32_t* width,
    uint32_t* height,
    void** texture_shared_handle,
    void** fence_shared_handle,
    uint64_t* producer_fence_value,
    uint64_t* consumer_fence_value,
    uint64_t* adapter_luid,
    uint32_t* rtp_timestamp,
    int64_t* timestamp_usec)
{
#if defined(_WIN32)
    if (!retained || !retained->frame.buffer || !width || !height ||
        !texture_shared_handle || !fence_shared_handle ||
        !producer_fence_value || !consumer_fence_value || !adapter_luid ||
        !rtp_timestamp)
        return 0;
    NPWindowsNativeNV12Surface surface;
    if (!np_get_windows_native_nv12_surface(
            retained->frame.buffer.get(), surface))
        return 0;
    *width = surface.width;
    *height = surface.height;
    *texture_shared_handle = surface.texture_shared_handle;
    *fence_shared_handle = surface.fence_shared_handle;
    *producer_fence_value = surface.producer_fence_value;
    *consumer_fence_value = surface.consumer_fence_value;
    *adapter_luid = surface.adapter_luid;
    *rtp_timestamp = retained->frame.rtp_timestamp;
    if (timestamp_usec)
        *timestamp_usec = surface.source_timestamp_usec > 0
            ? surface.source_timestamp_usec
            : retained->frame.timestamp_usec;
    return 1;
#else
    (void)retained; (void)width; (void)height; (void)texture_shared_handle;
    (void)fence_shared_handle; (void)producer_fence_value;
    (void)consumer_fence_value; (void)adapter_luid; (void)rtp_timestamp;
    (void)timestamp_usec;
    return 0;
#endif
}

extern "C" void np_webrtc_video_frame_mark_native_completion_scheduled(
    NPWebRTCVideoFrame* retained)
{
#if defined(_WIN32)
    if (retained && retained->frame.buffer)
        np_mark_windows_native_nv12_completion_scheduled(
            retained->frame.buffer.get());
#else
    (void)retained;
#endif
}

extern "C" void np_webrtc_video_frame_release(NPWebRTCVideoFrame* frame)
{
    delete frame;
}

extern "C" int np_webrtc_bridge_supports_native_nv12(NPWebRTCBridge* bridge)
{
    return bridge && bridge->session && bridge->session->SupportsNativeNV12()
        ? 1 : 0;
}

extern "C" void np_webrtc_bridge_report_native_nv12_failure(
    NPWebRTCBridge* bridge)
{
    if (bridge && bridge->session)
        bridge->session->ReportNativeNV12Failure();
}

extern "C" int np_webrtc_bridge_send_control(NPWebRTCBridge* bridge, const uint8_t* data, size_t size)
{
    return bridge && bridge->session && bridge->session->SendControl(data, size) ? 1 : 0;
}

extern "C" int np_webrtc_bridge_receive_control(
    NPWebRTCBridge* bridge, uint8_t* destination, size_t destination_capacity, size_t* required_size)
{
    if (!bridge || !bridge->session || !required_size)
        return 0;
    if (bridge->pending_control.empty() && !bridge->session->ReceiveControl(bridge->pending_control))
        return 0;
    *required_size = bridge->pending_control.size();
    if (!destination || destination_capacity < *required_size)
        return -1;
    std::memcpy(destination, bridge->pending_control.data(), *required_size);
    bridge->pending_control.clear();
    return 1;
}

extern "C" int np_webrtc_bridge_send_frame_metadata(
    NPWebRTCBridge* bridge, const uint8_t* data, size_t size)
{
    return bridge && bridge->session && bridge->session->SendFrameMetadata(data, size) ? 1 : 0;
}

extern "C" int np_webrtc_bridge_request_keyframe(NPWebRTCBridge* bridge)
{
    return bridge && bridge->session && bridge->session->RequestKeyframe() ? 1 : 0;
}

extern "C" int np_webrtc_bridge_receive_frame_metadata(
    NPWebRTCBridge* bridge, uint8_t* destination, size_t destination_capacity, size_t* required_size)
{
    if (!bridge || !bridge->session || !required_size)
        return 0;
    if (bridge->pending_frame_metadata.empty() &&
        !bridge->session->ReceiveFrameMetadata(bridge->pending_frame_metadata))
        return 0;
    *required_size = bridge->pending_frame_metadata.size();
    if (!destination || destination_capacity < *required_size)
        return -1;
    std::memcpy(destination, bridge->pending_frame_metadata.data(), *required_size);
    bridge->pending_frame_metadata.clear();
    return 1;
}

extern "C" void np_webrtc_bridge_get_stats(NPWebRTCBridge* bridge, NPWebRTCBridgeStats* stats)
{
    if (!stats)
        return;
    std::memset(stats, 0, sizeof(*stats));
    if (!bridge || !bridge->session)
    {
        stats->state = NP_WEBRTC_DISABLED;
        return;
    }
    stats->state = bridge->session->GetState();
    stats->sent_frames = bridge->session->SentFrames();
    stats->received_frames = bridge->session->ReceivedFrames();
    stats->dropped_frames = bridge->session->DroppedFrames();
    stats->decoded_queue_depth = bridge->session->DecodedQueueDepth();
    stats->sent_controls = bridge->session->SentControls();
    stats->received_controls = bridge->session->ReceivedControls();
    bridge->session->ReadCodecTelemetry(*stats);
    const std::string status = bridge->session->GetStatus();
    const size_t count = std::min(status.size(), sizeof(stats->status) - 1);
    std::memcpy(stats->status, status.data(), count);
}
