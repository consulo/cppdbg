#pragma once

#include <future>

namespace dap {
class Session;
}

namespace cppdbg {

class Engine;

// Wires up initialize / launch / configurationDone / continue / threads
// / disconnect on the given session. `disconnected` is fulfilled when
// the client requests disconnect — main() waits on its future to exit.
void registerLifecycleHandlers(dap::Session& session, Engine& engine,
                               std::promise<void>& disconnected);

}  // namespace cppdbg
