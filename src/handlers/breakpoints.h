#pragma once

namespace dap {
class Session;
}

namespace cppdbg {

class Engine;

void registerBreakpointHandlers(dap::Session& session, Engine& engine);

}  // namespace cppdbg
