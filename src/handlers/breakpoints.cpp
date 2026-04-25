#include "handlers/breakpoints.h"

#include <dap/protocol.h>
#include <dap/session.h>

#include <cstdlib>
#include <string>
#include <vector>

#include "engine.h"

namespace cppdbg {

namespace {

// Parse a memoryReference like "0x7ff..." or a plain decimal integer.
ULONG64 parseAddress(const std::string& s) {
    if (s.empty()) return 0;
    return std::strtoull(s.c_str(), nullptr,
                         (s.size() > 1 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
                             ? 16 : 0);
}

}  // namespace

void registerBreakpointHandlers(dap::Session& session, Engine& engine) {
    session.registerHandler(
        [&](const dap::SetBreakpointsRequest& req)
            -> dap::ResponseOrError<dap::SetBreakpointsResponse> {
            if (!req.source.path || req.source.path->empty())
                return dap::Error("setBreakpoints: source.path required");

            const std::string path = *req.source.path;
            std::vector<SourceBreakpointSpec> specs;
            if (req.breakpoints) {
                specs.reserve(req.breakpoints->size());
                for (const dap::SourceBreakpoint& sb : *req.breakpoints) {
                    SourceBreakpointSpec spec;
                    spec.line = static_cast<int>(sb.line);
                    if (sb.condition)    spec.condition = *sb.condition;
                    if (sb.hitCondition) spec.hitCondition = *sb.hitCondition;
                    if (sb.logMessage)   spec.logMessage = *sb.logMessage;
                    specs.push_back(std::move(spec));
                }
            } else if (req.lines) {
                specs.reserve(req.lines->size());
                for (dap::integer l : *req.lines) {
                    SourceBreakpointSpec spec;
                    spec.line = static_cast<int>(l);
                    specs.push_back(std::move(spec));
                }
            }

            std::vector<SourceBreakpoint> resolved;
            try {
                resolved = engine.commands()
                               .post([&] {
                                   return engine.setSourceBreakpoints(path,
                                                                      specs);
                               })
                               .get();
            } catch (const std::exception& ex) {
                return dap::Error(ex.what());
            }

            dap::SetBreakpointsResponse resp;
            resp.breakpoints.reserve(resolved.size());
            for (const SourceBreakpoint& r : resolved) {
                dap::Breakpoint b;
                b.verified = r.verified;
                if (r.verified) {
                    b.line = static_cast<dap::integer>(r.actualLine);
                    b.source = req.source;
                    b.id = static_cast<dap::integer>(r.engineId);
                }
                resp.breakpoints.push_back(b);
            }
            return resp;
        });

    session.registerHandler(
        [&](const dap::SetFunctionBreakpointsRequest& req)
            -> dap::ResponseOrError<dap::SetFunctionBreakpointsResponse> {
            std::vector<FunctionBreakpointSpec> specs;
            specs.reserve(req.breakpoints.size());
            for (const dap::FunctionBreakpoint& fb : req.breakpoints) {
                FunctionBreakpointSpec s;
                s.name = fb.name;
                if (fb.condition)    s.condition = *fb.condition;
                if (fb.hitCondition) s.hitCondition = *fb.hitCondition;
                specs.push_back(std::move(s));
            }
            std::vector<SourceBreakpoint> resolved;
            try {
                resolved = engine.commands()
                               .post([&] {
                                   return engine.setFunctionBreakpoints(specs);
                               })
                               .get();
            } catch (const std::exception& ex) {
                return dap::Error(ex.what());
            }
            dap::SetFunctionBreakpointsResponse resp;
            resp.breakpoints.reserve(resolved.size());
            for (const SourceBreakpoint& r : resolved) {
                dap::Breakpoint b;
                b.verified = r.verified;
                if (r.verified) b.id = static_cast<dap::integer>(r.engineId);
                resp.breakpoints.push_back(b);
            }
            return resp;
        });

    session.registerHandler(
        [&](const dap::SetExceptionBreakpointsRequest& req)
            -> dap::ResponseOrError<dap::SetExceptionBreakpointsResponse> {
            std::vector<std::string> filters;
            filters.reserve(req.filters.size());
            for (const dap::string& f : req.filters) filters.push_back(f);
            // filterOptions (with conditions) — not yet supported.
            try {
                engine.commands()
                    .post([&engine, filters] {
                        engine.setExceptionBreakpoints(filters);
                    })
                    .get();
            } catch (const std::exception& ex) {
                return dap::Error(ex.what());
            }
            return dap::SetExceptionBreakpointsResponse{};
        });

    session.registerHandler(
        [&](const dap::DisassembleRequest& req)
            -> dap::ResponseOrError<dap::DisassembleResponse> {
            const ULONG64 base = parseAddress(req.memoryReference);
            const int byteOffset = req.offset
                ? static_cast<int>(*req.offset) : 0;
            const int instrOffset = req.instructionOffset
                ? static_cast<int>(*req.instructionOffset) : 0;
            const int count = static_cast<int>(req.instructionCount);
            const bool resolveSymbols = req.resolveSymbols
                ? static_cast<bool>(*req.resolveSymbols) : true;

            // Apply byte offset directly. instructionOffset is documented
            // as variable-length-instruction-aware; on x64 we can't easily
            // walk backwards, so we approximate by treating it as bytes
            // when negative — most clients only use it with 0/positive.
            const ULONG64 start = base + static_cast<int64_t>(byteOffset)
                                  + static_cast<int64_t>(instrOffset);
            std::vector<Engine::DisassembledInstruction> raw;
            try {
                raw = engine.commands()
                          .post([&] {
                              return engine.disassemble(start, count,
                                                        resolveSymbols);
                          })
                          .get();
            } catch (const std::exception& ex) {
                return dap::Error(ex.what());
            }
            dap::DisassembleResponse resp;
            resp.instructions.reserve(raw.size());
            for (const auto& r : raw) {
                dap::DisassembledInstruction di;
                di.address = r.address;
                di.instruction = r.instruction;
                if (!r.symbol.empty()) di.symbol = r.symbol;
                resp.instructions.push_back(di);
            }
            return resp;
        });
}

}  // namespace cppdbg
