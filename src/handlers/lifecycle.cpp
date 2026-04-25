#include "handlers/lifecycle.h"

#include <dap/protocol.h>
#include <dap/session.h>
#include <dap/typeof.h>

#include <future>
#include <string>

#include "engine.h"
#include "util/wstring.h"

namespace cppdbg {

// Client-specific launch arguments. Standard DAP leaves `launch` to
// adapters; we accept `program`, `args`, `cwd`.
class CppdbgLaunchRequest : public dap::LaunchRequest {
public:
    dap::string program;
    dap::optional<dap::array<dap::string>> args;
    dap::optional<dap::string> cwd;
};

// Attach by OS process id. Adapter-specific.
class CppdbgAttachRequest : public dap::AttachRequest {
public:
    dap::integer processId = 0;
};

}  // namespace cppdbg

namespace dap {
DAP_DECLARE_STRUCT_TYPEINFO(cppdbg::CppdbgLaunchRequest);
DAP_IMPLEMENT_STRUCT_TYPEINFO_EXT(cppdbg::CppdbgLaunchRequest,
                                  dap::LaunchRequest, "launch",
                                  DAP_FIELD(program, "program"),
                                  DAP_FIELD(args, "args"),
                                  DAP_FIELD(cwd, "cwd"));
DAP_DECLARE_STRUCT_TYPEINFO(cppdbg::CppdbgAttachRequest);
DAP_IMPLEMENT_STRUCT_TYPEINFO_EXT(cppdbg::CppdbgAttachRequest,
                                  dap::AttachRequest, "attach",
                                  DAP_FIELD(processId, "processId"));
}  // namespace dap

namespace cppdbg {

namespace {

std::wstring joinArgs(const dap::optional<dap::array<dap::string>>& args) {
    if (!args) return {};
    std::wstring out;
    for (const dap::string& a : *args) {
        if (!out.empty()) out += L' ';
        // Quote every argument — callers shouldn't have to double-quote
        // paths with spaces in launch.json.
        out += L'"';
        out += to_wide(a);
        out += L'"';
    }
    return out;
}

}  // namespace

void registerLifecycleHandlers(dap::Session& session, Engine& engine,
                               std::promise<void>& disconnected) {
    session.registerHandler([](const dap::InitializeRequest&) {
        dap::InitializeResponse r;
        r.supportsConfigurationDoneRequest = true;
        r.supportsSetVariable = true;
        r.supportsEvaluateForHovers = true;
        r.supportsConditionalBreakpoints = true;
        r.supportsHitConditionalBreakpoints = true;
        r.supportsLogPoints = true;
        r.supportsFunctionBreakpoints = true;
        r.supportsDisassembleRequest = true;
        // Exception filters. We expose only "firstChance" because
        // second-chance / unhandled exceptions always break — they're
        // process crashes that the user always wants to see.
        dap::ExceptionBreakpointsFilter firstChance;
        firstChance.filter = "firstChance";
        firstChance.label = "First-chance exceptions";
        firstChance.description = "Break when any exception is thrown, "
            "before the target's own handlers run.";
        firstChance.def = false;
        r.exceptionBreakpointFilters = dap::array<dap::ExceptionBreakpointsFilter>{firstChance};
        return r;
    });

    session.registerSentHandler(
        [&](const dap::ResponseOrError<dap::InitializeResponse>&) {
            session.send(dap::InitializedEvent{});
        });

    session.registerHandler(
        [&](const CppdbgLaunchRequest& req)
            -> dap::ResponseOrError<dap::LaunchResponse> {
            if (req.program.empty())
                return dap::Error("launch: 'program' is required");
            try {
                auto program = to_wide(req.program);
                auto args = joinArgs(req.args);
                auto cwd = req.cwd ? to_wide(*req.cwd) : std::wstring{};
                engine.commands()
                    .post([&engine, program, args, cwd] {
                        engine.launch(program, args, cwd);
                    })
                    .get();
            } catch (const std::exception& ex) {
                return dap::Error(ex.what());
            }
            return dap::LaunchResponse{};
        });

    session.registerHandler(
        [&](const CppdbgAttachRequest& req)
            -> dap::ResponseOrError<dap::AttachResponse> {
            if (req.processId <= 0)
                return dap::Error("attach: 'processId' is required");
            const ULONG pid = static_cast<ULONG>(req.processId);
            try {
                engine.commands()
                    .post([&engine, pid] { engine.attach(pid); })
                    .get();
            } catch (const std::exception& ex) {
                return dap::Error(ex.what());
            }
            return dap::AttachResponse{};
        });

    session.registerHandler(
        [&](const dap::ConfigurationDoneRequest&)
            -> dap::ResponseOrError<dap::ConfigurationDoneResponse> {
            try {
                engine.commands().post([&] { engine.resume(); }).get();
            } catch (const std::exception& ex) {
                return dap::Error(ex.what());
            }
            return dap::ConfigurationDoneResponse{};
        });

    session.registerHandler(
        [&](const dap::ContinueRequest&)
            -> dap::ResponseOrError<dap::ContinueResponse> {
            try {
                engine.commands().post([&] { engine.resume(); }).get();
            } catch (const std::exception& ex) {
                return dap::Error(ex.what());
            }
            dap::ContinueResponse r;
            r.allThreadsContinued = true;
            return r;
        });

    session.registerHandler(
        [&, sigFired = std::make_shared<std::atomic<bool>>(false)](
            const dap::DisconnectRequest& req)
            -> dap::ResponseOrError<dap::DisconnectResponse> {
            const bool terminate = req.terminateDebuggee.value(false);
            try {
                engine.commands()
                    .post([&, terminate] {
                        engine.detachOrTerminate(terminate);
                    })
                    .get();
            } catch (const std::exception& ex) {
                return dap::Error(ex.what());
            }
            if (!sigFired->exchange(true)) disconnected.set_value();
            return dap::DisconnectResponse{};
        });
}

}  // namespace cppdbg
