#include "handlers/stepping.h"

#include <dap/protocol.h>
#include <dap/session.h>

#include "engine.h"

namespace cppdbg {

void registerSteppingHandlers(dap::Session& session, Engine& engine) {
    session.registerHandler(
        [&](const dap::NextRequest& req)
            -> dap::ResponseOrError<dap::NextResponse> {
            const int tid = static_cast<int>(req.threadId);
            try {
                engine.commands().post([&, tid] { engine.stepOver(tid); }).get();
            } catch (const std::exception& ex) {
                return dap::Error(ex.what());
            }
            return dap::NextResponse{};
        });

    session.registerHandler(
        [&](const dap::StepInRequest& req)
            -> dap::ResponseOrError<dap::StepInResponse> {
            const int tid = static_cast<int>(req.threadId);
            try {
                engine.commands().post([&, tid] { engine.stepInto(tid); }).get();
            } catch (const std::exception& ex) {
                return dap::Error(ex.what());
            }
            return dap::StepInResponse{};
        });

    session.registerHandler(
        [&](const dap::StepOutRequest& req)
            -> dap::ResponseOrError<dap::StepOutResponse> {
            const int tid = static_cast<int>(req.threadId);
            try {
                engine.commands().post([&, tid] { engine.stepOut(tid); }).get();
            } catch (const std::exception& ex) {
                return dap::Error(ex.what());
            }
            return dap::StepOutResponse{};
        });

    session.registerHandler(
        [&](const dap::PauseRequest&)
            -> dap::ResponseOrError<dap::PauseResponse> {
            // pause is unique: it must NOT block on the engine queue,
            // because the engine thread is parked inside WaitForEvent
            // while the target is running, and it cannot pop tasks until
            // the target breaks. SetInterrupt is documented as safe to
            // call from any thread, so dispatch it directly.
            try {
                engine.pause();
            } catch (const std::exception& ex) {
                return dap::Error(ex.what());
            }
            return dap::PauseResponse{};
        });
}

}  // namespace cppdbg
