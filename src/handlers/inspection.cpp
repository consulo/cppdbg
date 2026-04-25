#include "handlers/inspection.h"

#include <dap/protocol.h>
#include <dap/session.h>

#include <cstdio>
#include <string>

#include "engine.h"

namespace cppdbg {

namespace {

// All four inspection requests bridge a DAP handler call into the engine
// thread via commands().post(...).get(). Wrap that in a helper so each
// handler stays a flat data-shape transformation.
template <class Fn>
auto runOnEngine(Engine& engine, Fn&& fn) -> decltype(fn()) {
    return engine.commands().post(std::forward<Fn>(fn)).get();
}

}  // namespace

void registerInspectionHandlers(dap::Session& session, Engine& engine) {
    session.registerHandler(
        [&](const dap::ThreadsRequest&)
            -> dap::ResponseOrError<dap::ThreadsResponse> {
            std::vector<DapThread> threads;
            try {
                threads = runOnEngine(engine, [&] { return engine.getThreads(); });
            } catch (const std::exception& ex) {
                return dap::Error(ex.what());
            }
            dap::ThreadsResponse r;
            for (const DapThread& t : threads) {
                dap::Thread dt;
                dt.id = t.id;
                dt.name = t.name;
                r.threads.push_back(dt);
            }
            return r;
        });

    session.registerHandler(
        [&](const dap::StackTraceRequest& req)
            -> dap::ResponseOrError<dap::StackTraceResponse> {
            const int threadId = static_cast<int>(req.threadId);
            const int startFrame =
                req.startFrame ? static_cast<int>(*req.startFrame) : 0;
            const int levels =
                req.levels ? static_cast<int>(*req.levels) : 0;

            std::vector<DapFrame> frames;
            try {
                frames = runOnEngine(engine, [&] {
                    return engine.getStackTrace(threadId, startFrame, levels);
                });
            } catch (const std::exception& ex) {
                return dap::Error(ex.what());
            }

            dap::StackTraceResponse r;
            for (const DapFrame& f : frames) {
                dap::StackFrame sf;
                sf.id = f.id;
                sf.name = f.name;
                sf.line = f.line;
                sf.column = 0;
                if (!f.sourcePath.empty()) {
                    dap::Source src;
                    src.path = f.sourcePath;
                    sf.source = src;
                }
                // Memory reference for the frame's PC. Clients pass this
                // straight to `disassemble` — works on x64 *and* arm64
                // without needing to evaluate `@rip` / `@pc`.
                char ipBuf[32];
                std::snprintf(ipBuf, sizeof(ipBuf), "0x%016llx",
                              static_cast<unsigned long long>(
                                  f.instructionOffset));
                sf.instructionPointerReference = std::string{ipBuf};
                r.stackFrames.push_back(sf);
            }
            r.totalFrames = static_cast<dap::integer>(frames.size());
            return r;
        });

    session.registerHandler(
        [&](const dap::ScopesRequest& req)
            -> dap::ResponseOrError<dap::ScopesResponse> {
            const int frameId = static_cast<int>(req.frameId);
            std::vector<DapScope> scopes;
            try {
                scopes = runOnEngine(engine,
                                     [&] { return engine.getScopes(frameId); });
            } catch (const std::exception& ex) {
                return dap::Error(ex.what());
            }
            dap::ScopesResponse r;
            for (const DapScope& s : scopes) {
                dap::Scope ds;
                ds.name = s.name;
                ds.variablesReference = s.variablesReference;
                ds.expensive = s.expensive;
                if (!s.presentationHint.empty())
                    ds.presentationHint = s.presentationHint;
                r.scopes.push_back(ds);
            }
            return r;
        });

    session.registerHandler(
        [&](const dap::VariablesRequest& req)
            -> dap::ResponseOrError<dap::VariablesResponse> {
            const int varRef = static_cast<int>(req.variablesReference);
            std::vector<DapVariable> vars;
            try {
                vars = runOnEngine(engine,
                                   [&] { return engine.getVariables(varRef); });
            } catch (const std::exception& ex) {
                return dap::Error(ex.what());
            }
            dap::VariablesResponse r;
            for (const DapVariable& v : vars) {
                dap::Variable dv;
                dv.name = v.name;
                dv.value = v.value;
                if (!v.type.empty()) dv.type = v.type;
                dv.variablesReference = v.variablesReference;
                r.variables.push_back(dv);
            }
            return r;
        });

    session.registerHandler(
        [&](const dap::EvaluateRequest& req)
            -> dap::ResponseOrError<dap::EvaluateResponse> {
            const int frameId =
                req.frameId ? static_cast<int>(*req.frameId) : 0;
            const std::string expression = req.expression;
            const std::string context =
                req.context ? *req.context : std::string{};
            DapEvaluateResult res;
            try {
                res = runOnEngine(engine, [&] {
                    return engine.evaluate(frameId, expression, context);
                });
            } catch (const std::exception& ex) {
                return dap::Error(ex.what());
            }
            dap::EvaluateResponse r;
            r.result = res.result;
            r.variablesReference = res.variablesReference;
            if (!res.type.empty()) r.type = res.type;
            return r;
        });

    session.registerHandler(
        [&](const dap::SetVariableRequest& req)
            -> dap::ResponseOrError<dap::SetVariableResponse> {
            const int varRef = static_cast<int>(req.variablesReference);
            const std::string name = req.name;
            const std::string value = req.value;
            DapVariable updated;
            try {
                updated = runOnEngine(engine, [&] {
                    return engine.setVariable(varRef, name, value);
                });
            } catch (const std::exception& ex) {
                return dap::Error(ex.what());
            }
            dap::SetVariableResponse r;
            r.value = updated.value;
            if (!updated.type.empty()) r.type = updated.type;
            return r;
        });
}

}  // namespace cppdbg
