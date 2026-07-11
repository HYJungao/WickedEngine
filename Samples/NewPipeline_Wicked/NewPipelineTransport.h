#pragma once

#include "NewPipelineProtocol.h"

#include <array>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace wicked_newpipeline
{
struct RemoteRawBuffer
{
    RemoteBufferSemantic semantic = RemoteBufferSemantic::RemoteIndirectDiffuse;
    uint32_t width = 0;
    uint32_t height = 0;
    bool available = false;
    std::vector<uint8_t> payload_rgba8;
};

struct RemoteRawFrame
{
    RemoteFrameMetadata metadata;
    std::array<RemoteRawBuffer, static_cast<size_t>(RemoteBufferSemantic::Count)> buffers = {};

    RemoteRawFrame();
    RemoteRawBuffer* FindBuffer(RemoteBufferSemantic semantic);
    const RemoteRawBuffer* FindBuffer(RemoteBufferSemantic semantic) const;
};

struct PackedRemoteVideoFrame
{
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> i420;
};

// Packs all four semantic buffers and all frame-bound metadata into one I420 video frame.
// No downstream frame payload or identity metadata is carried by a data channel.
bool EncodeRemoteVideoFrame(const RemoteRawFrame& frame, PackedRemoteVideoFrame& video, std::string* error = nullptr);
bool DecodeRemoteVideoFrame(const PackedRemoteVideoFrame& video, RemoteRawFrame& frame, std::string* error = nullptr);

class IRemoteTransport
{
public:
    virtual ~IRemoteTransport() = default;

    virtual RemoteSourceMode GetSourceMode() const = 0;
};

class NullRemoteTransport final : public IRemoteTransport
{
public:
    explicit NullRemoteTransport(RemoteSourceMode mode) : source_mode(mode) {}

    RemoteSourceMode GetSourceMode() const override { return source_mode; }

private:
    RemoteSourceMode source_mode = RemoteSourceMode::Mock;
};

class InProcessControlMailbox final
{
public:
    void Publish(const ClientControlPacket& packet);
    bool TryConsumeLatest(ClientControlPacket& packet);
    bool PeekLatest(ClientControlPacket& packet) const;
    void Reset();

private:
    mutable std::mutex     mutex;
    ClientControlPacket    latest_packet;
    uint64_t               latest_sequence  = 0;
    uint64_t               consumed_sequence = 0;
};

InProcessControlMailbox& GetInProcessControlMailbox();

std::string GetDefaultMockMailboxDirectory();
std::string GetDefaultMockRemoteMailboxDirectory();
std::string GetDefaultMockControlMailboxDirectory();

class FileMockControlMailbox final
{
public:
    explicit FileMockControlMailbox(std::string root_directory = GetDefaultMockControlMailboxDirectory());

    const std::string& GetRootDirectory() const { return root_directory; }

    bool PublishLatest(const ClientControlPacket& packet, std::string* error = nullptr) const;
    bool TryConsumeLatest(ClientControlPacket& packet, std::string* error = nullptr);

private:
    std::string root_directory;
    uint64_t consumed_frame_id = 0;
};

class FileMockRemoteMailbox final
{
public:
    explicit FileMockRemoteMailbox(std::string root_directory = GetDefaultMockRemoteMailboxDirectory());

    const std::string& GetRootDirectory() const { return root_directory; }

    bool PublishLatest(const RemoteRawFrame& frame, std::string* error = nullptr) const;
    bool TryReadLatest(RemoteRawFrame& frame, std::string* error = nullptr) const;

private:
    std::string root_directory;
};

enum class WebRTCTransportState : uint8_t
{
    Disabled,
    Starting,
    Signaling,
    Connected,
    Failed,
};

struct WebRTCTransportStats
{
    WebRTCTransportState state = WebRTCTransportState::Disabled;
    uint64_t sent_frames = 0;
    uint64_t received_frames = 0;
    uint64_t dropped_frames = 0;
    uint64_t sent_controls = 0;
    uint64_t received_controls = 0;
    std::string status;
};

class WebRTCVideoTransport final : public IRemoteTransport
{
public:
    WebRTCVideoTransport();
    ~WebRTCVideoTransport() override;
    WebRTCVideoTransport(const WebRTCVideoTransport&) = delete;
    WebRTCVideoTransport& operator=(const WebRTCVideoTransport&) = delete;

    RemoteSourceMode GetSourceMode() const override { return RemoteSourceMode::WebRTC; }
    bool Start(bool server, const RuntimeConfig& config, std::string* error = nullptr);
    void Stop();
    void Tick();
    bool SendControl(const ClientControlPacket& packet);
    bool TryReceiveControl(ClientControlPacket& packet);
    bool SendFrame(const RemoteRawFrame& frame);
    bool TryReceiveFrame(RemoteRawFrame& frame);
    WebRTCTransportStats GetStats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace wicked_newpipeline
