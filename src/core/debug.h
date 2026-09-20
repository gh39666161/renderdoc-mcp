#pragma once

#include "core/types.h"
#include "core/session.h"
#include <optional>

namespace renderdoc::core {

// `captureVariables` fills ShaderDebugResult::variables with the final value of
// every variable written during execution (texture sample results,
// intermediates, outputs). Much cheaper than a full trace, and the practical
// way to diff the same shader between two captures.
ShaderDebugResult debugPixel(
    const Session& session,
    uint32_t eventId,
    uint32_t x, uint32_t y,
    bool fullTrace = false,
    uint32_t primitive = 0xFFFFFFFF,
    bool captureVariables = false);

ShaderDebugResult debugVertex(
    const Session& session,
    uint32_t eventId,
    uint32_t vertexId,
    bool fullTrace = false,
    uint32_t instance = 0,
    uint32_t index = 0xFFFFFFFF,
    uint32_t view = 0,
    bool captureVariables = false);

ShaderDebugResult debugThread(
    const Session& session,
    uint32_t eventId,
    uint32_t groupX, uint32_t groupY, uint32_t groupZ,
    uint32_t threadX, uint32_t threadY, uint32_t threadZ,
    bool fullTrace = false,
    bool captureVariables = false);

} // namespace renderdoc::core
