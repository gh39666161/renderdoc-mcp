#pragma once

#include "core/types.h"
#include "core/shader_edit.h"
#include <string>
#include <vector>

// Forward declarations from RenderDoc
struct ICaptureFile;
struct IReplayController;
struct IRemoteServer;

namespace renderdoc::core {

class Session {
public:
    Session();
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // Public API
    CaptureInfo open(const std::string& path);
    CaptureInfo open(const std::string& path, const std::string& remoteHost);
    // Open a capture that already exists on the remote device (skips copy).
    CaptureInfo openRemotePath(const std::string& devicePath, const std::string& remoteHost);
    void close();
    SessionStatus status() const;
    bool isOpen() const;
    void ensureReplayInitialized();

    // Remote device connection (Android / adb / TCP remote server)
    std::vector<RemoteDevice> listDevices();
    RemoteDevice connectDevice(const std::string& host, bool startServer = true);
    void disconnectDevice();
    bool isRemoteReplay() const;
    const std::string& remoteHost() const;

    // Internal accessors for other core modules.
    // Convention: mcp/cli layers should NOT call these directly.
    IReplayController* controller() const;
    ICaptureFile* captureFile() const;
    uint32_t currentEventId() const;
    const std::string& capturePath() const;
    std::string exportDir() const;
    ShaderEditState& shaderEditState();
    const ShaderEditState& shaderEditState() const;

private:
    friend EventInfo gotoEvent(Session& session, uint32_t eventId);

    void setCurrentEventId(uint32_t eid);
    void closeCurrent();
    void disconnectRemote();
    std::string normalizeRemoteHost(const std::string& host);
    CaptureInfo gatherCaptureInfo(const std::string& path);

    ICaptureFile* m_captureFile = nullptr;
    IReplayController* m_controller = nullptr;
    IRemoteServer* m_remote = nullptr;
    uint32_t m_currentEventId = 0;
    std::string m_capturePath;
    std::string m_remoteHost;
    bool m_replayInitialized = false;
    uint32_t m_totalEvents = 0;
    GraphicsApi m_api = GraphicsApi::Unknown;
    ShaderEditState m_shaderEditState;
};

} // namespace renderdoc::core
