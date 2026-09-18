#include "mcp/tools/tools.h"
#include "mcp/tool_registry.h"
#include "mcp/serialization.h"
#include "core/session.h"

namespace renderdoc::mcp::tools {

void registerSessionTools(ToolRegistry& registry) {
    registry.registerTool({
        "open_capture",
        "Open a RenderDoc capture file (.rdc) for analysis. Returns the graphics API type "
        "and total event/draw counts. Closes any previously opened capture. "
        "Optional remoteHost replays the capture on a phone or remote machine "
        "(e.g. adb://SERIAL). Use list_devices to discover connected Android devices.",
        {{"type", "object"},
         {"properties", {
             {"path", {{"type", "string"},
                       {"description", "Absolute path to the .rdc capture file"}}},
             {"remoteHost", {{"type", "string"},
                             {"description", "Remote replay host. Android serial, "
                                             "adb://SERIAL, or host:port of a running "
                                             "renderdoc remote server. Empty = local replay "
                                             "or the previously connected device."}}}
         }},
         {"required", {"path"}}},
        [](mcp::ToolContext& ctx, const nlohmann::json& args) -> nlohmann::json {
            auto& session = ctx.session;
            auto path = args["path"].get<std::string>();
            auto remote = args.value("remoteHost", "");
            auto info = remote.empty() ? session.open(path) : session.open(path, remote);
            auto j = to_json(info);
            j["remoteHost"] = session.remoteHost();
            j["remoteReplay"] = session.isRemoteReplay();
            return j;
        }
    });

    registry.registerTool({
        "list_devices",
        "Enumerate connected remote replay devices (Android phones via adb, etc.). "
        "Call this before connecting so RenderDoc can initialise ADB port forwards. "
        "Returns host URLs such as adb://SERIAL for use with connect_device / open_capture.",
        {{"type", "object"}, {"properties", nlohmann::json::object()}},
        [](mcp::ToolContext& ctx, const nlohmann::json&) -> nlohmann::json {
            auto devices = ctx.session.listDevices();
            nlohmann::json j;
            j["devices"] = to_json_array(devices);
            j["count"] = devices.size();
            return j;
        }
    });

    registry.registerTool({
        "connect_device",
        "Connect to a remote RenderDoc server for on-device replay. For Android, pass "
        "the serial or adb://SERIAL; this will install/start org.renderdoc.renderdoccmd "
        "if needed. Subsequent open_capture calls replay on this device until disconnect_device.",
        {{"type", "object"},
         {"properties", {
             {"host", {{"type", "string"},
                       {"description", "Device host: Android serial, adb://SERIAL, "
                                       "or host:port of a remote server"}}},
             {"startServer", {{"type", "boolean"},
                              {"description", "Start the RenderDoc remote server on the "
                                              "device if it is not already running. Default true"}}}
         }},
         {"required", {"host"}}},
        [](mcp::ToolContext& ctx, const nlohmann::json& args) -> nlohmann::json {
            auto host = args["host"].get<std::string>();
            bool startServer = args.value("startServer", true);
            auto device = ctx.session.connectDevice(host, startServer);
            return to_json(device);
        }
    });

    registry.registerTool({
        "disconnect_device",
        "Close any open capture and disconnect from the remote replay device.",
        {{"type", "object"}, {"properties", nlohmann::json::object()}},
        [](mcp::ToolContext& ctx, const nlohmann::json&) -> nlohmann::json {
            ctx.session.disconnectDevice();
            return {{"status", "disconnected"}};
        }
    });

    registry.registerTool({
        "close_capture",
        "Close the currently opened capture and release resources.",
        {{"type", "object"}, {"properties", nlohmann::json::object()}},
        [](mcp::ToolContext& ctx, const nlohmann::json&) -> nlohmann::json {
            ctx.session.close();
            return {{"status", "closed"}};
        }
    });

    registry.registerTool({
        "session_status",
        "Query whether a capture is currently open. Returns session state including "
        "capture path, API type, current event ID, total events, and remote replay host.",
        {{"type", "object"}, {"properties", nlohmann::json::object()}},
        [](mcp::ToolContext& ctx, const nlohmann::json&) -> nlohmann::json {
            return to_json(ctx.session.status());
        }
    });
}

} // namespace renderdoc::mcp::tools
