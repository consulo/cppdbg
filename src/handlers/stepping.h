#pragma once

namespace dap {
class Session;
}

namespace cppdbg {

class Engine;

// next / stepIn / stepOut / pause.
void registerSteppingHandlers(dap::Session& session, Engine& engine);

}  // namespace cppdbg
