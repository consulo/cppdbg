#pragma once

namespace dap {
class Session;
}

namespace cppdbg {

class Engine;

// Threads, stack frames, scopes, and (read-only) variables.
void registerInspectionHandlers(dap::Session& session, Engine& engine);

}  // namespace cppdbg
