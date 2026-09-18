#include "core/session.h"
#include "core/action_helpers.h"
#include "core/shader_edit.h"
#include "core/errors.h"

// RenderDoc headers — guarded by RENDERDOC_DIR at build time
#include <renderdoc_replay.h>

namespace renderdoc::core {

namespace {

std::string toStd(const rdcstr& s) {
    return std::string(s.c_str());
}

std::string protocolFromHost(const std::string& host) {
    auto pos = host.find("://");
    if (pos == std::string::npos)
        return "";
    return host.substr(0, pos);
}

std::string deviceIdFromHost(const std::string& host) {
    auto pos = host.find("://");
    if (pos == std::string::npos)
        return host;
    return host.substr(pos + 3);
}

bool looksLikeAdbSerial(const std::string& host) {
    if (host.find("://") != std::string::npos)
        return false;
    if (host == "localhost" || host == "127.0.0.1")
        return false;
    // Bare hostname without a port is treated as an adb serial.
    // host:port is a TCP remote server (renderdoccmd remoteserver).
    // Wireless adb (IP:PORT) must be passed as adb://IP:PORT.
    if (host.find(':') != std::string::npos)
        return false;
    return !host.empty();
}

RemoteDevice makeDevice(IDeviceProtocolController* proto, const std::string& host) {
    RemoteDevice d;
    d.host = host;
    d.protocol = proto ? toStd(proto->GetProtocolName()) : protocolFromHost(host);
    if (proto) {
        d.name = toStd(proto->GetFriendlyName(rdcstr(host.c_str())));
        d.supported = proto->IsSupported(rdcstr(host.c_str()));
    } else {
        d.name = host;
        d.supported = true;
    }
    if (d.name.empty())
        d.name = deviceIdFromHost(host);

    auto check = RENDERDOC_CheckRemoteServerConnection(rdcstr(host.c_str()));
    if (check.code == ResultCode::Succeeded) {
        d.serverRunning = true;
        d.busy = false;
        d.status = "ready";
    } else if (check.code == ResultCode::NetworkRemoteBusy) {
        d.serverRunning = true;
        d.busy = true;
        d.status = "busy";
    } else if (check.code == ResultCode::NetworkVersionMismatch) {
        d.serverRunning = true;
        d.busy = true;
        d.status = "version-mismatch: " + toStd(check.Message());
    } else {
        d.serverRunning = false;
        d.busy = false;
        d.status = d.supported ? "offline" : "unsupported";
    }
    return d;
}

} // anonymous namespace

Session::Session() = default;

Session::~Session() {
    close();
    disconnectRemote();
    if (m_replayInitialized)
        RENDERDOC_ShutdownReplay();
}

void Session::ensureReplayInitialized() {
    if (m_replayInitialized)
        return;
    GlobalEnvironment env;
    memset(&env, 0, sizeof(env));
    rdcarray<rdcstr> args;
    RENDERDOC_InitialiseReplay(env, args);
    m_replayInitialized = true;
}

void Session::disconnectRemote() {
    if (m_remote) {
        m_remote->ShutdownConnection();
        m_remote = nullptr;
    }
    m_remoteHost.clear();
}

void Session::closeCurrent() {
    if (m_controller) {
        cleanupShaderEdits(*this);
        if (m_remote) {
            m_remote->CloseCapture(m_controller);
        } else {
            m_controller->Shutdown();
        }
        m_controller = nullptr;
    }
    if (m_captureFile) {
        m_captureFile->Shutdown();
        m_captureFile = nullptr;
    }
    m_currentEventId = 0;
    m_capturePath.clear();
    m_totalEvents = 0;
    m_api = GraphicsApi::Unknown;
    m_shaderEditState.clear();
}

void Session::close() {
    closeCurrent();
}

std::string Session::normalizeRemoteHost(const std::string& host) {
    if (host.empty())
        return {};

    std::string h = host;
    // Bare adb serial -> adb://SERIAL
    if (looksLikeAdbSerial(h))
        h = "adb://" + h;

    // If this is a protocol URL, enumerate devices first so RenderDoc
    // assigns port forwards (required before CreateRemoteServerConnection).
    std::string protoName = protocolFromHost(h);
    if (!protoName.empty()) {
        auto* proto = RENDERDOC_GetDeviceProtocolController(rdcstr(protoName.c_str()));
        if (proto)
            proto->GetDevices();
    }
    return h;
}

std::vector<RemoteDevice> Session::listDevices() {
    ensureReplayInitialized();

    std::vector<RemoteDevice> out;

    rdcarray<rdcstr> protocols;
    RENDERDOC_GetSupportedDeviceProtocols(&protocols);

    for (const auto& p : protocols) {
        auto* proto = RENDERDOC_GetDeviceProtocolController(p);
        if (!proto)
            continue;
        auto devices = proto->GetDevices();
        std::string protoName = toStd(proto->GetProtocolName());
        for (const auto& d : devices) {
            std::string host = protoName + "://" + toStd(d);
            out.push_back(makeDevice(proto, host));
        }
    }

    return out;
}

RemoteDevice Session::connectDevice(const std::string& host, bool startServer) {
    ensureReplayInitialized();
    closeCurrent();
    disconnectRemote();

    std::string resolved = normalizeRemoteHost(host);
    if (resolved.empty())
        throw CoreError(CoreError::Code::DeviceNotFound, "Device host is empty");

    std::string protoName = protocolFromHost(resolved);
    IDeviceProtocolController* proto = nullptr;
    if (!protoName.empty())
        proto = RENDERDOC_GetDeviceProtocolController(rdcstr(protoName.c_str()));

    if (proto) {
        auto devices = proto->GetDevices();
        std::string wantId = deviceIdFromHost(resolved);
        bool found = false;
        for (const auto& d : devices) {
            if (toStd(d) == wantId) {
                found = true;
                break;
            }
        }
        if (!found) {
            throw CoreError(CoreError::Code::DeviceNotFound,
                            "Device not found: " + resolved +
                            ". Call list_devices after connecting the phone via adb.");
        }

        if (!proto->IsSupported(rdcstr(resolved.c_str()))) {
            throw CoreError(CoreError::Code::RemoteConnectionFailed,
                            "Device is not supported for RenderDoc replay: " + resolved);
        }
    }

    auto check = RENDERDOC_CheckRemoteServerConnection(rdcstr(resolved.c_str()));
    bool needStart = startServer &&
                     check.code != ResultCode::Succeeded &&
                     check.code != ResultCode::NetworkRemoteBusy &&
                     check.code != ResultCode::NetworkVersionMismatch;

    if (needStart) {
        if (!proto) {
            throw CoreError(CoreError::Code::RemoteConnectionFailed,
                            "Remote server is not running on " + resolved +
                            ". Start renderdoccmd remoteserver on the target, or use an adb:// device.");
        }
        auto started = proto->StartRemoteServer(rdcstr(resolved.c_str()));
        if (!started.OK() &&
            started.code != ResultCode::AndroidGrantPermissionsFailed &&
            started.code != ResultCode::AndroidAPKVerifyFailed) {
            throw CoreError(CoreError::Code::RemoteConnectionFailed,
                            "Failed to start remote server on " + resolved + ": " +
                            toStd(started.Message()));
        }
        check = RENDERDOC_CheckRemoteServerConnection(rdcstr(resolved.c_str()));
    }

    if (check.code == ResultCode::NetworkVersionMismatch) {
        throw CoreError(CoreError::Code::RemoteConnectionFailed,
                        "RenderDoc version mismatch with " + resolved + ": " +
                        toStd(check.Message()));
    }

    IRemoteServer* remote = nullptr;
    auto conn = RENDERDOC_CreateRemoteServerConnection(rdcstr(resolved.c_str()), &remote);
    if (!remote || !conn.OK()) {
        throw CoreError(CoreError::Code::RemoteConnectionFailed,
                        "Failed to connect to " + resolved + ": " + toStd(conn.Message()));
    }

    m_remote = remote;
    m_remoteHost = resolved;

    RemoteDevice info = makeDevice(proto, resolved);
    info.serverRunning = true;
    info.status = "connected";
    return info;
}

void Session::disconnectDevice() {
    closeCurrent();
    disconnectRemote();
}

bool Session::isRemoteReplay() const {
    return m_remote != nullptr;
}

const std::string& Session::remoteHost() const {
    return m_remoteHost;
}

CaptureInfo Session::gatherCaptureInfo(const std::string& path) {
    auto apiProps = m_controller->GetAPIProperties();
    m_api = toGraphicsApi(apiProps.pipelineType);

    const auto& rootActions = m_controller->GetRootActions();
    m_totalEvents = 0;
    uint32_t totalDraws = 0;
    for (const auto& action : rootActions) {
        m_totalEvents += countAllEvents(action);
        totalDraws += countDrawCalls(action);
    }

    CaptureInfo info;
    info.path = path;
    info.api = m_api;
    info.degraded = apiProps.degraded;
    info.totalEvents = m_totalEvents;
    info.totalDraws = totalDraws;
    return info;
}

CaptureInfo Session::open(const std::string& path) {
    return open(path, m_remoteHost);
}

CaptureInfo Session::open(const std::string& path, const std::string& remoteHost) {
    ensureReplayInitialized();
    closeCurrent();

    if (!remoteHost.empty()) {
        std::string resolved = normalizeRemoteHost(remoteHost);
        if (resolved != m_remoteHost)
            connectDevice(resolved, true);
    }

    if (m_remote) {
        rdcstr remotePath = m_remote->CopyCaptureToRemote(rdcstr(path.c_str()), nullptr);
        if (remotePath.empty()) {
            throw CoreError(CoreError::Code::ReplayInitFailed,
                            "Failed to copy capture to remote device: " + path);
        }

        ReplayOptions opts;
        auto [replayStatus, controller] =
            m_remote->OpenCapture(IRemoteServer::NoPreference, remotePath, opts, nullptr);
        if (!replayStatus.OK() || !controller) {
            throw CoreError(CoreError::Code::ReplayInitFailed,
                            "Failed to open remote replay on " + m_remoteHost + ": " +
                            toStd(replayStatus.Message()));
        }

        m_controller = controller;
        m_capturePath = path;
        return gatherCaptureInfo(path);
    }

    m_captureFile = RENDERDOC_OpenCaptureFile();
    if (!m_captureFile)
        throw CoreError(CoreError::Code::InternalError, "Failed to create capture file object");

    auto status = m_captureFile->OpenFile(rdcstr(path.c_str()), "", nullptr);
    if (!status.OK()) {
        m_captureFile->Shutdown();
        m_captureFile = nullptr;
        throw CoreError(CoreError::Code::FileNotFound,
                        "Failed to open capture: " + toStd(status.Message()));
    }

    ReplayOptions opts;
    auto [replayStatus, controller] = m_captureFile->OpenCapture(opts, nullptr);
    if (!replayStatus.OK() || !controller) {
        m_captureFile->Shutdown();
        m_captureFile = nullptr;
        throw CoreError(CoreError::Code::ReplayInitFailed,
                        "Failed to open replay: " + toStd(replayStatus.Message()));
    }

    m_controller = controller;
    m_capturePath = path;
    return gatherCaptureInfo(path);
}

SessionStatus Session::status() const {
    SessionStatus s;
    s.isOpen = isOpen();
    s.capturePath = m_capturePath;
    s.api = m_api;
    s.currentEventId = m_currentEventId;
    s.totalEvents = m_totalEvents;
    s.remoteHost = m_remoteHost;
    s.remoteReplay = m_remote != nullptr;
    return s;
}

bool Session::isOpen() const {
    return m_controller != nullptr;
}

IReplayController* Session::controller() const {
    if (!m_controller)
        throw CoreError(CoreError::Code::NoCaptureOpen, "No capture is currently open");
    return m_controller;
}

ICaptureFile* Session::captureFile() const {
    return m_captureFile;
}

uint32_t Session::currentEventId() const { return m_currentEventId; }
const std::string& Session::capturePath() const { return m_capturePath; }

std::string Session::exportDir() const {
    if (m_capturePath.empty()) return ".";
    auto pos = m_capturePath.find_last_of("\\/");
    std::string dir = (pos != std::string::npos) ? m_capturePath.substr(0, pos) : ".";
    return dir + "/renderdoc-mcp-export";
}

void Session::setCurrentEventId(uint32_t eid) {
    m_currentEventId = eid;
}

ShaderEditState& Session::shaderEditState() { return m_shaderEditState; }
const ShaderEditState& Session::shaderEditState() const { return m_shaderEditState; }

} // namespace renderdoc::core
