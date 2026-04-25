#include "engine.h"

#include <dap/protocol.h>
#include <dap/session.h>

#include <string>

#include "util/hr.h"
#include "util/wstring.h"

namespace cppdbg {

namespace {

const char* stopReasonString(StopReason reason) {
    switch (reason) {
        case StopReason::Entry:      return "entry";
        case StopReason::Breakpoint: return "breakpoint";
        case StopReason::Exception:  return "exception";
        case StopReason::Step:       return "step";
        case StopReason::Pause:      return "pause";
    }
    return "unknown";
}

std::wstring buildCommandLine(const std::wstring& program,
                              const std::wstring& args) {
    // DbgEng parses CommandLine as a debugger directive — forward slashes
    // in the exe path make it mis-tokenise. Normalise to backslashes.
    std::wstring normalised = program;
    for (wchar_t& c : normalised) {
        if (c == L'/') c = L'\\';
    }
    std::wstring out;
    out.reserve(normalised.size() + args.size() + 4);
    out += L'"';
    out += normalised;
    out += L'"';
    if (!args.empty()) {
        out += L' ';
        out += args;
    }
    return out;
}

// Snapshot the current PC's source line + enclosing function name. Used
// to detect when an instruction-level step has reached a *different*
// source line / function — that's our DAP step-completion criterion.
void snapshotStepAnchor(IDebugRegisters2* registers, IDebugSymbols3* symbols,
                        ULONG& outLine, std::wstring& outFunc) {
    outLine = 0;
    outFunc.clear();
    ULONG64 pc = 0;
    if (registers && FAILED(registers->GetInstructionOffset(&pc))) return;
    if (!symbols) return;
    symbols->GetLineByOffsetWide(pc, &outLine, nullptr, 0, nullptr, nullptr);
    wchar_t name[512] = {};
    ULONG nameSize = 0;
    if (SUCCEEDED(symbols->GetNameByOffsetWide(pc, name, 512, &nameSize, nullptr))
        && nameSize > 1) {
        outFunc.assign(name, nameSize - 1);
    }
}

}  // namespace

Engine::Engine() = default;

Engine::~Engine() {
    stop();
}

void Engine::start() {
    if (running_.exchange(true)) return;
    thread_ = std::thread([this] { threadMain(); });
}

void Engine::stop() {
    if (!running_.exchange(false)) return;
    queue_.stop();
    if (thread_.joinable()) thread_.join();
}

void Engine::threadMain() {
    initCom();
    try {
        while (running_.load()) {
            if (targetRunning_) {
                waitForNextEvent();
                continue;
            }
            CommandQueue::Task task;
            // Block briefly so we notice `running_` flips without a posted
            // task (e.g., disconnect during an idle session).
            if (queue_.tryPop(task, 50)) {
                task();
            }
        }
    } catch (const std::exception& ex) {
        if (session_) {
            dap::OutputEvent out;
            out.category = "stderr";
            out.output = std::string("engine: fatal: ") + ex.what() + "\n";
            session_->send(out);
        }
    }
    shutdownCom();
}

void Engine::initCom() {
    CPPDBG_HR(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    CPPDBG_HR(DebugCreate(__uuidof(IDebugClient5),
                          reinterpret_cast<void**>(&client_)));
    CPPDBG_HR(client_->QueryInterface(__uuidof(IDebugControl5),
                                      reinterpret_cast<void**>(&control_)));
    CPPDBG_HR(client_->QueryInterface(__uuidof(IDebugSymbols3),
                                      reinterpret_cast<void**>(&symbols_)));
    CPPDBG_HR(client_->QueryInterface(
        IID_IDebugSystemObjects,
        reinterpret_cast<void**>(&system_)));
    CPPDBG_HR(client_->QueryInterface(
        __uuidof(IDebugRegisters2),
        reinterpret_cast<void**>(&registers_)));
    CPPDBG_HR(client_->SetEventCallbacksWide(&callbacks_));
    CPPDBG_HR(control_->SetEngineOptions(DEBUG_ENGOPT_INITIAL_BREAK));
}

void Engine::shutdownCom() {
    if (client_) {
        client_->SetEventCallbacksWide(nullptr);
        if (processLive_) {
            client_->TerminateProcesses();
            client_->EndSession(DEBUG_END_ACTIVE_TERMINATE);
        }
    }
    if (registers_) { registers_->Release(); registers_ = nullptr; }
    if (system_)  { system_->Release();  system_  = nullptr; }
    if (symbols_) { symbols_->Release(); symbols_ = nullptr; }
    if (control_) { control_->Release(); control_ = nullptr; }
    if (client_)  { client_->Release();  client_  = nullptr; }
    CoUninitialize();
}

void Engine::waitForNextEvent() {
    HRESULT hr = control_->WaitForEvent(0, INFINITE);
    if (FAILED(hr)) {
        targetRunning_ = false;
        if (hr == E_UNEXPECTED && !processLive_) return;
        throw hr_error(hr, "WaitForEvent");
    }
    // STEP_INTO / STEP_OVER complete silently. If no callback fired
    // (targetRunning_ still true) and the engine is back in BREAK, that
    // was an instruction-step completion. Either keep stepping (still
    // on the original source line) or surface stopped(step).
    if (targetRunning_ && stepKind_ != StepKind::None) {
        ULONG status = 0;
        if (SUCCEEDED(control_->GetExecutionStatus(&status))
            && status == DEBUG_STATUS_BREAK) {
            ULONG nowLine = 0;
            std::wstring nowFunc;
            snapshotStepAnchor(registers_, symbols_, nowLine, nowFunc);
            const bool moved = (nowFunc != stepStartFunc_)
                || (nowLine != stepStartLine_ && nowLine != 0);
            if (moved) {
                StepKind k = stepKind_;
                stepKind_ = StepKind::None;
                stepStartFunc_.clear();
                stepStartLine_ = 0;
                (void)k;
                targetRunning_ = false;
                emitStopped(StopReason::Step, {});
            } else {
                // Same line, same function — keep instruction-stepping.
                ULONG next = (stepKind_ == StepKind::Into)
                    ? DEBUG_STATUS_STEP_INTO : DEBUG_STATUS_STEP_OVER;
                control_->SetExecutionStatus(next);
                // targetRunning_ stays true — main loop loops into Wait.
            }
        }
    }
}

void Engine::launch(const std::wstring& program,
                    const std::wstring& args,
                    const std::wstring& cwd) {
    std::wstring cmdLine = buildCommandLine(program, args);

    DEBUG_CREATE_PROCESS_OPTIONS opts = {};
    opts.CreateFlags = DEBUG_PROCESS | DEBUG_ONLY_THIS_PROCESS;
    opts.EngCreateFlags = DEBUG_ECREATE_PROCESS_DEFAULT;
    CPPDBG_HR(client_->CreateProcess2Wide(
        0, cmdLine.data(), &opts, sizeof(opts),
        cwd.empty() ? nullptr : cwd.c_str(), nullptr));
    processLive_ = true;

    // Pump the initial loader break (an OS STATUS_BREAKPOINT), which is
    // suppressed by onException() while entryPending_ is set.
    entryPending_ = true;
    targetRunning_ = true;
    waitForNextEvent();

    // Set a one-shot breakpoint at the executable entry point. $exentry
    // is a DbgEng pseudo-register resolving to the PE AddressOfEntryPoint
    // — available as soon as the image is mapped, which it is by now.
    DEBUG_VALUE entryVal = {};
    CPPDBG_HR(control_->EvaluateWide(L"$exentry", DEBUG_VALUE_INT64,
                                     &entryVal, nullptr));

    IDebugBreakpoint2* bp = nullptr;
    CPPDBG_HR(control_->AddBreakpoint2(DEBUG_BREAKPOINT_CODE, DEBUG_ANY_ID,
                                       &bp));
    CPPDBG_HR(bp->SetOffset(entryVal.I64));
    CPPDBG_HR(bp->AddFlags(DEBUG_BREAKPOINT_ENABLED |
                           DEBUG_BREAKPOINT_ONE_SHOT));
    CPPDBG_HR(bp->GetId(&entryBreakpointId_));

    // Drive from the loader break to the entry point, so that by the
    // time launch() returns, the main module's PDB has been loaded and
    // setBreakpoints can resolve source/line to addresses. onBreakpoint
    // will emit the stopped(entry) DAP event when $exentry hits.
    CPPDBG_HR(control_->SetExecutionStatus(DEBUG_STATUS_GO));
    targetRunning_ = true;
    waitForNextEvent();
}

void Engine::attach(ULONG processId) {
    if (processLive_) throw std::runtime_error("attach: already attached");

    // Same `entryPending_` flag as launch — the attach handshake produces
    // a STATUS_BREAKPOINT exception (DbgEng injects DbgBreakPoint into
    // the target), which onException suppresses while this is set.
    entryPending_ = true;
    targetRunning_ = true;

    CPPDBG_HR(client_->AttachProcess(0, processId, DEBUG_ATTACH_DEFAULT));
    processLive_ = true;

    // Pump the attach break.
    waitForNextEvent();

    // Mark for deferred stopped(entry) emission via configurationDone.
    // (No $exentry breakpoint to set — the target is already past entry.)
    entryPending_ = false;
    entryStopPending_ = true;
}

void Engine::resume() {
    if (!processLive_) return;
    if (entryStopPending_) {
        // First "resume" after launch is configurationDone; surface the
        // stopped(entry) event we deferred and stay paused. The user's
        // subsequent continue lands on the SetExecutionStatus path below.
        entryStopPending_ = false;
        emitStopped(StopReason::Entry, {});
        return;
    }
    CPPDBG_HR(control_->SetExecutionStatus(DEBUG_STATUS_GO));
    targetRunning_ = true;
}

namespace {

// Parse hitCondition as a positive integer ("5" → 5). Returns 0 if the
// string isn't a clean positive int — caller treats that as "no hit
// limit". DAP allows richer expressions like ">5"/"%5"/"=5"; we only
// support the bare-N form for now and silently ignore the rest.
ULONG parseHitCount(const std::string& s) {
    if (s.empty()) return 0;
    size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    if (i < s.size() && s[i] == '=') ++i;
    ULONG n = 0;
    bool any = false;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
        n = n * 10 + static_cast<ULONG>(s[i] - '0');
        any = true;
        ++i;
    }
    return any ? n : 0;
}

}  // namespace

std::vector<SourceBreakpoint> Engine::setSourceBreakpoints(
    const std::string& sourcePath,
    const std::vector<SourceBreakpointSpec>& specs) {

    // Clear previous BPs for this source.
    if (auto it = sourceBpIds_.find(sourcePath); it != sourceBpIds_.end()) {
        for (ULONG id : it->second) {
            IDebugBreakpoint2* bp = nullptr;
            if (SUCCEEDED(control_->GetBreakpointById2(id, &bp)) && bp) {
                control_->RemoveBreakpoint2(bp);
            }
            bpAttrs_.erase(id);
        }
        sourceBpIds_.erase(it);
    }

    // After launch we're past entry so PDBs are typically already loaded;
    // after attach we still need to ask the engine to walk the module
    // table. `/f` forces immediate symbol load (vs deferred).
    if (symbols_) symbols_->ReloadWide(L"/f");

    // PDBs store paths with backslashes; DbgEng's GetOffsetByLineWide
    // does literal substring matching and does NOT tolerate forward
    // slashes. DAP clients (VS Code on Windows, nvim-dap) often emit
    // forward slashes — normalise on the way in.
    std::wstring pathW = to_wide(sourcePath);
    for (wchar_t& c : pathW) if (c == L'/') c = L'\\';

    std::vector<SourceBreakpoint> result;
    std::vector<ULONG> ids;
    result.reserve(specs.size());

    for (const SourceBreakpointSpec& spec : specs) {
        SourceBreakpoint r;
        r.requestedLine = spec.line;
        r.actualLine = spec.line;

        ULONG64 offset = 0;
        HRESULT hrL = symbols_->GetOffsetByLineWide(
            static_cast<ULONG>(spec.line), pathW.c_str(), &offset);
        if (SUCCEEDED(hrL)) {
            IDebugBreakpoint2* bp = nullptr;
            HRESULT hrAdd = control_->AddBreakpoint2(
                DEBUG_BREAKPOINT_CODE, DEBUG_ANY_ID, &bp);
            if (SUCCEEDED(hrAdd) && bp) {
                bp->SetOffset(offset);
                bp->AddFlags(DEBUG_BREAKPOINT_ENABLED);
                if (ULONG passCount = parseHitCount(spec.hitCondition);
                    passCount > 1) {
                    // DbgEng's PassCount=N fires the BP on the Nth hit
                    // (counter decrements with each pass; break when it
                    // reaches 0 — i.e., after N total hits).
                    bp->SetPassCount(passCount);
                }
                ULONG id = 0;
                bp->GetId(&id);
                r.engineId = id;
                r.verified = true;
                if (!spec.condition.empty() || !spec.logMessage.empty()) {
                    bpAttrs_[id] =
                        BpAttrs{spec.condition, spec.logMessage};
                }
                ULONG resolvedLine = 0;
                if (SUCCEEDED(symbols_->GetLineByOffsetWide(
                        offset, &resolvedLine, nullptr, 0, nullptr, nullptr))
                    && resolvedLine) {
                    r.actualLine = static_cast<int>(resolvedLine);
                }
                ids.push_back(id);
            }
        }
        result.push_back(r);
    }

    if (!ids.empty()) sourceBpIds_[sourcePath] = std::move(ids);
    return result;
}

std::vector<SourceBreakpoint> Engine::setFunctionBreakpoints(
    const std::vector<FunctionBreakpointSpec>& specs) {

    // Clear previous function BPs.
    for (ULONG id : funcBpIds_) {
        IDebugBreakpoint2* bp = nullptr;
        if (SUCCEEDED(control_->GetBreakpointById2(id, &bp)) && bp) {
            control_->RemoveBreakpoint2(bp);
        }
        bpAttrs_.erase(id);
    }
    funcBpIds_.clear();

    if (symbols_) symbols_->ReloadWide(L"/f");

    std::vector<SourceBreakpoint> result;
    result.reserve(specs.size());

    for (const FunctionBreakpointSpec& spec : specs) {
        SourceBreakpoint r;
        r.requestedLine = 0;
        r.actualLine = 0;
        if (spec.name.empty()) {
            result.push_back(r);
            continue;
        }
        IDebugBreakpoint2* bp = nullptr;
        HRESULT hrAdd = control_->AddBreakpoint2(
            DEBUG_BREAKPOINT_CODE, DEBUG_ANY_ID, &bp);
        if (SUCCEEDED(hrAdd) && bp) {
            // SetOffsetExpression resolves a symbolic name and survives
            // module load/unload cycles — exactly what a function BP
            // needs in DAP.
            const std::wstring nameW = to_wide(spec.name);
            HRESULT hrExpr = bp->SetOffsetExpressionWide(nameW.c_str());
            if (SUCCEEDED(hrExpr)) {
                bp->AddFlags(DEBUG_BREAKPOINT_ENABLED);
                if (ULONG passCount = parseHitCount(spec.hitCondition);
                    passCount > 1) {
                    bp->SetPassCount(passCount - 1);
                }
                ULONG id = 0;
                bp->GetId(&id);
                r.engineId = id;
                r.verified = true;
                if (!spec.condition.empty()) {
                    bpAttrs_[id] = BpAttrs{spec.condition, {}};
                }
                funcBpIds_.push_back(id);
            } else {
                control_->RemoveBreakpoint2(bp);
            }
        }
        result.push_back(r);
    }
    return result;
}

std::vector<Engine::DisassembledInstruction> Engine::disassemble(
    ULONG64 address, int count, bool resolveSymbols) {
    std::vector<DisassembledInstruction> out;
    if (!control_ || !symbols_ || count <= 0) return out;

    ULONG64 cur = address;
    for (int i = 0; i < count; ++i) {
        wchar_t buf[256] = {};
        ULONG written = 0;
        ULONG64 next = 0;
        HRESULT hr = control_->DisassembleWide(
            cur, DEBUG_DISASM_EFFECTIVE_ADDRESS, buf, 256, &written, &next);
        DisassembledInstruction inst;
        char addrBuf[32];
        std::snprintf(addrBuf, sizeof(addrBuf), "0x%016llx",
                      static_cast<unsigned long long>(cur));
        inst.address = addrBuf;

        if (SUCCEEDED(hr) && written) {
            // DbgEng output is "address  bytes  mnemonic operands\n";
            // strip the trailing newline and the leading address (it's
            // already in inst.address).
            std::string line = to_utf8(buf);
            while (!line.empty()
                   && (line.back() == '\n' || line.back() == '\r')) {
                line.pop_back();
            }
            inst.instruction = line;

            if (resolveSymbols) {
                wchar_t name[256] = {};
                ULONG nameSz = 0;
                ULONG64 disp = 0;
                if (SUCCEEDED(symbols_->GetNameByOffsetWide(
                        cur, name, 256, &nameSz, &disp)) && nameSz > 1) {
                    inst.symbol = to_utf8(name);
                    if (disp) {
                        char dbuf[32];
                        std::snprintf(dbuf, sizeof(dbuf), "+0x%llx",
                                      static_cast<unsigned long long>(disp));
                        inst.symbol += dbuf;
                    }
                }
            }
            cur = next ? next : cur + 1;
        } else {
            inst.instruction = "??";
            cur += 1;
        }
        out.push_back(std::move(inst));
    }
    return out;
}

void Engine::stepOver(int threadEngineId) {
    if (!processLive_) return;
    if (system_) system_->SetCurrentThreadId(static_cast<ULONG>(threadEngineId));
    snapshotStepAnchor(registers_, symbols_, stepStartLine_, stepStartFunc_);
    stepKind_ = StepKind::Over;
    CPPDBG_HR(control_->SetExecutionStatus(DEBUG_STATUS_STEP_OVER));
    targetRunning_ = true;
}

void Engine::stepInto(int threadEngineId) {
    if (!processLive_) return;
    if (system_) system_->SetCurrentThreadId(static_cast<ULONG>(threadEngineId));
    snapshotStepAnchor(registers_, symbols_, stepStartLine_, stepStartFunc_);
    stepKind_ = StepKind::Into;
    CPPDBG_HR(control_->SetExecutionStatus(DEBUG_STATUS_STEP_INTO));
    targetRunning_ = true;
}

void Engine::stepOut(int threadEngineId) {
    if (!processLive_) return;
    if (system_) system_->SetCurrentThreadId(static_cast<ULONG>(threadEngineId));

    // Find the caller's return address and arm a one-shot BP there. This
    // is exactly what WinDbg's `gu` (go up) does.
    DEBUG_STACK_FRAME_EX frames[2] = {};
    ULONG filled = 0;
    CPPDBG_HR(control_->GetStackTraceEx(0, 0, 0, frames, 2, &filled));
    if (filled < 1 || !frames[0].ReturnOffset)
        throw std::runtime_error("stepOut: cannot determine return address");

    IDebugBreakpoint2* bp = nullptr;
    CPPDBG_HR(control_->AddBreakpoint2(DEBUG_BREAKPOINT_CODE, DEBUG_ANY_ID,
                                       &bp));
    bp->SetOffset(frames[0].ReturnOffset);
    bp->AddFlags(DEBUG_BREAKPOINT_ENABLED | DEBUG_BREAKPOINT_ONE_SHOT);
    bp->GetId(&stepOutBpId_);

    CPPDBG_HR(control_->SetExecutionStatus(DEBUG_STATUS_GO));
    targetRunning_ = true;
}

void Engine::pause() {
    if (!processLive_) return;
    pauseRequested_ = true;
    // SetInterrupt is one of the few DbgEng methods safe to call while
    // the engine thread is blocked in WaitForEvent — it goes through the
    // engine's interrupt request channel.
    if (control_) control_->SetInterrupt(DEBUG_INTERRUPT_ACTIVE);
}

void Engine::detachOrTerminate(bool terminate) {
    if (!processLive_) return;
    if (terminate) {
        client_->TerminateProcesses();
        client_->EndSession(DEBUG_END_ACTIVE_TERMINATE);
    } else {
        client_->DetachProcesses();
        client_->EndSession(DEBUG_END_ACTIVE_DETACH);
    }
    processLive_ = false;
    targetRunning_ = false;
}

ULONG Engine::currentThreadId() {
    ULONG id = 0;
    if (system_) system_->GetCurrentThreadId(&id);
    // DbgEng thread ids are engine-local (0..N); DAP uses them opaquely.
    return id;
}

void Engine::emitStopped(StopReason reason, const std::string& description) {
    invalidateInspectionCaches();
    if (!session_) return;
    dap::StoppedEvent event;
    event.reason = stopReasonString(reason);
    event.threadId = static_cast<dap::integer>(currentThreadId());
    event.allThreadsStopped = true;
    if (!description.empty()) event.description = description;
    session_->send(event);
}

void Engine::invalidateInspectionCaches() {
    for (auto& [_, group] : frameGroups_) {
        if (group) group->Release();
    }
    for (auto& [_, group] : synthGroups_) {
        if (group) group->Release();
    }
    frameGroups_.clear();
    synthGroups_.clear();
    frames_.clear();
    varRefs_.clear();
    nextFrameId_ = 1;
    nextVarRef_ = 1;
}

// ---- M3 inspection ----------------------------------------------------

std::vector<DapThread> Engine::getThreads() {
    std::vector<DapThread> out;
    if (!system_) return out;
    ULONG count = 0;
    if (FAILED(system_->GetNumberThreads(&count)) || count == 0) return out;
    std::vector<ULONG> ids(count), sysIds(count);
    if (FAILED(system_->GetThreadIdsByIndex(0, count, ids.data(),
                                            sysIds.data()))) return out;
    out.reserve(count);
    for (ULONG i = 0; i < count; ++i) {
        DapThread t;
        t.id = static_cast<int>(ids[i]);
        char buf[64];
        std::snprintf(buf, sizeof(buf), "Thread %lu (sysid %lu)",
                      static_cast<unsigned long>(ids[i]),
                      static_cast<unsigned long>(sysIds[i]));
        t.name = buf;
        out.push_back(std::move(t));
    }
    return out;
}

std::vector<DapFrame> Engine::getStackTrace(int threadEngineId, int startFrame,
                                            int levels) {
    std::vector<DapFrame> out;
    if (!system_ || !control_ || !symbols_) return out;
    if (FAILED(system_->SetCurrentThreadId(
            static_cast<ULONG>(threadEngineId)))) return out;

    constexpr ULONG kMax = 256;
    DEBUG_STACK_FRAME_EX raw[kMax] = {};
    ULONG filled = 0;
    if (FAILED(control_->GetStackTraceEx(0, 0, 0, raw, kMax, &filled)))
        return out;

    int end = static_cast<int>(filled);
    if (levels > 0 && startFrame + levels < end) end = startFrame + levels;
    if (startFrame < 0) startFrame = 0;

    out.reserve(static_cast<size_t>(end - startFrame));
    for (int i = startFrame; i < end; ++i) {
        DapFrame f;
        f.id = nextFrameId_++;
        f.instructionOffset = raw[i].InstructionOffset;

        wchar_t nameBuf[512] = {};
        ULONG nameSize = 0;
        ULONG64 disp = 0;
        if (SUCCEEDED(symbols_->GetNameByOffsetWide(
                raw[i].InstructionOffset, nameBuf, 512, &nameSize, &disp))
            && nameSize > 1) {
            f.name = to_utf8(nameBuf);
            if (disp) {
                char dispBuf[32];
                std::snprintf(dispBuf, sizeof(dispBuf), "+0x%llx",
                              static_cast<unsigned long long>(disp));
                f.name += dispBuf;
            }
        } else {
            char addrBuf[32];
            std::snprintf(addrBuf, sizeof(addrBuf), "0x%016llx",
                          static_cast<unsigned long long>(
                              raw[i].InstructionOffset));
            f.name = addrBuf;
        }

        wchar_t fileBuf[1024] = {};
        ULONG line = 0;
        ULONG fileSize = 0;
        if (SUCCEEDED(symbols_->GetLineByOffsetWide(
                raw[i].InstructionOffset, &line, fileBuf, 1024, &fileSize,
                nullptr)) && line) {
            f.line = static_cast<int>(line);
            f.sourcePath = to_utf8(fileBuf);
        }

        // DbgEng's GetStackTraceEx emits both an INLINE (physical-IP)
        // frame and a STACK (physical-IP) frame for any function that
        // had compiler inlining at the call site. When name+line+IP are
        // identical the two are user-indistinguishable, so collapse to
        // one entry. Real inlining (different callee names) is kept.
        if (!out.empty()) {
            const DapFrame& prev = out.back();
            if (prev.instructionOffset == f.instructionOffset
                && prev.name == f.name
                && prev.line == f.line) {
                --nextFrameId_;  // give the id back, we're skipping this one
                continue;
            }
        }

        CachedFrame cf;
        cf.threadEngineId = static_cast<ULONG>(threadEngineId);
        cf.frameIndex = static_cast<ULONG>(i);
        cf.instructionOffset = raw[i].InstructionOffset;
        frames_[f.id] = cf;

        out.push_back(std::move(f));
    }
    return out;
}

std::vector<DapScope> Engine::getScopes(int frameId) {
    std::vector<DapScope> out;
    if (!frames_.count(frameId)) return out;
    DapScope s;
    s.name = "Locals";
    s.presentationHint = "locals";
    s.variablesReference = nextVarRef_++;
    varRefs_[s.variablesReference] = VarRefBinding{frameId, ~0u};
    out.push_back(std::move(s));
    return out;
}

IDebugSymbolGroup2* Engine::groupForVarRef(int varRef) {
    auto vit = varRefs_.find(varRef);
    if (vit == varRefs_.end()) return nullptr;
    const int frameId = vit->second.frameId;
    if (frameId == 0) {
        // Synthetic group from evaluate(); keyed by the same varRef.
        auto sit = synthGroups_.find(varRef);
        return sit == synthGroups_.end() ? nullptr : sit->second;
    }
    auto fit = frames_.find(frameId);
    if (fit == frames_.end()) return nullptr;
    IDebugSymbolGroup2*& fg = frameGroups_[frameId];
    if (!fg) {
        if (FAILED(system_->SetCurrentThreadId(
                fit->second.threadEngineId))) return nullptr;
        if (FAILED(symbols_->SetScopeFrameByIndex(
                fit->second.frameIndex))) return nullptr;
        if (FAILED(symbols_->GetScopeSymbolGroup2(
                DEBUG_SCOPE_GROUP_ALL, nullptr, &fg))) return nullptr;
    }
    return fg;
}

std::vector<DapVariable> Engine::getVariables(int variablesReference) {
    std::vector<DapVariable> out;
    auto vit = varRefs_.find(variablesReference);
    if (vit == varRefs_.end()) return out;
    const int frameId = vit->second.frameId;
    const ULONG parentIdx = vit->second.parentIdx;

    IDebugSymbolGroup2* group = groupForVarRef(variablesReference);
    if (!group) return out;

    ULONG total = 0;
    if (FAILED(group->GetNumberSymbols(&total))) return out;

    ULONG parentDepth = 0;
    if (parentIdx != ~0u) {
        DEBUG_SYMBOL_PARAMETERS pp = {};
        if (FAILED(group->GetSymbolParameters(parentIdx, 1, &pp))) return out;
        parentDepth = pp.Flags & DEBUG_SYMBOL_EXPANSION_LEVEL_MASK;
        if (!(pp.Flags & DEBUG_SYMBOL_EXPANDED)) {
            group->ExpandSymbol(parentIdx, TRUE);
            if (FAILED(group->GetNumberSymbols(&total))) return out;
        }
    }

    const ULONG startIdx = (parentIdx == ~0u) ? 0 : parentIdx + 1;
    const ULONG wantDepth = (parentIdx == ~0u) ? 0 : parentDepth + 1;

    for (ULONG i = startIdx; i < total; ++i) {
        DEBUG_SYMBOL_PARAMETERS pp = {};
        if (FAILED(group->GetSymbolParameters(i, 1, &pp))) continue;
        const ULONG depth = pp.Flags & DEBUG_SYMBOL_EXPANSION_LEVEL_MASK;
        if (parentIdx != ~0u && depth <= parentDepth) break;  // past parent
        if (depth != wantDepth) continue;                     // skip nested

        DapVariable v;
        wchar_t nameBuf[256] = {};
        ULONG nameSize = 0;
        if (SUCCEEDED(group->GetSymbolNameWide(i, nameBuf, 256, &nameSize))
            && nameSize > 1) {
            v.name = to_utf8(nameBuf);
        }
        wchar_t typeBuf[256] = {};
        ULONG typeSize = 0;
        if (SUCCEEDED(group->GetSymbolTypeNameWide(i, typeBuf, 256,
                                                   &typeSize))
            && typeSize > 1) {
            v.type = to_utf8(typeBuf);
        }
        wchar_t valBuf[1024] = {};
        ULONG valSize = 0;
        if (SUCCEEDED(group->GetSymbolValueTextWide(i, valBuf, 1024,
                                                    &valSize))
            && valSize > 1) {
            v.value = to_utf8(valBuf);
        }

        if (pp.SubElements > 0) {
            int childRef = nextVarRef_++;
            varRefs_[childRef] = VarRefBinding{frameId, i};
            v.variablesReference = childRef;
        }

        out.push_back(std::move(v));
    }
    return out;
}

DapVariable Engine::setVariable(int variablesReference,
                                const std::string& name,
                                const std::string& value) {
    auto vit = varRefs_.find(variablesReference);
    if (vit == varRefs_.end())
        throw std::runtime_error("setVariable: unknown variablesReference");
    const ULONG parentIdx = vit->second.parentIdx;

    IDebugSymbolGroup2* group = groupForVarRef(variablesReference);
    if (!group)
        throw std::runtime_error(
            "setVariable: cannot resolve symbol group for varRef");

    ULONG total = 0;
    if (FAILED(group->GetNumberSymbols(&total)))
        throw std::runtime_error("setVariable: GetNumberSymbols failed");

    ULONG parentDepth = 0;
    ULONG startIdx = 0;
    if (parentIdx != ~0u) {
        DEBUG_SYMBOL_PARAMETERS pp = {};
        if (FAILED(group->GetSymbolParameters(parentIdx, 1, &pp)))
            throw std::runtime_error("setVariable: parent params failed");
        parentDepth = pp.Flags & DEBUG_SYMBOL_EXPANSION_LEVEL_MASK;
        if (!(pp.Flags & DEBUG_SYMBOL_EXPANDED)) {
            group->ExpandSymbol(parentIdx, TRUE);
            group->GetNumberSymbols(&total);
        }
        startIdx = parentIdx + 1;
    }
    const ULONG wantDepth = (parentIdx == ~0u) ? 0 : parentDepth + 1;

    const std::wstring nameW = to_wide(name);
    ULONG foundIdx = ~0u;
    for (ULONG i = startIdx; i < total; ++i) {
        DEBUG_SYMBOL_PARAMETERS pp = {};
        if (FAILED(group->GetSymbolParameters(i, 1, &pp))) continue;
        const ULONG depth = pp.Flags & DEBUG_SYMBOL_EXPANSION_LEVEL_MASK;
        if (parentIdx != ~0u && depth <= parentDepth) break;
        if (depth != wantDepth) continue;
        wchar_t buf[256] = {};
        ULONG sz = 0;
        if (SUCCEEDED(group->GetSymbolNameWide(i, buf, 256, &sz)) && sz > 1
            && nameW == buf) {
            foundIdx = i;
            break;
        }
    }
    if (foundIdx == ~0u)
        throw std::runtime_error("setVariable: '" + name + "' not in scope");

    const std::wstring valueW = to_wide(value);
    HRESULT hr = group->WriteSymbolWide(foundIdx,
                                        const_cast<PWSTR>(valueW.c_str()));
    if (FAILED(hr)) throw hr_error(hr, "WriteSymbolWide");

    // Read back the canonical text representation the engine settled on
    // (e.g. "0n42" for an int set from "42").
    DapVariable result;
    result.name = name;
    wchar_t valBuf[1024] = {};
    ULONG valSize = 0;
    if (SUCCEEDED(group->GetSymbolValueTextWide(foundIdx, valBuf, 1024,
                                                &valSize))
        && valSize > 1) {
        result.value = to_utf8(valBuf);
    } else {
        result.value = value;
    }
    wchar_t typeBuf[256] = {};
    ULONG typeSize = 0;
    if (SUCCEEDED(group->GetSymbolTypeNameWide(foundIdx, typeBuf, 256,
                                               &typeSize))
        && typeSize > 1) {
        result.type = to_utf8(typeBuf);
    }
    return result;
}

DapEvaluateResult Engine::evaluate(int frameId,
                                   const std::string& expression,
                                   const std::string& /*context*/) {
    // The DAP `context` ("repl" / "watch" / "hover" / "clipboard") is
    // currently unused — our single symbol-group path renders all of
    // them identically. Kept in the signature for forward compatibility.
    DapEvaluateResult r;
    if (!control_ || !symbols_)
        throw std::runtime_error("evaluate: engine not initialised");
    if (expression.empty())
        throw std::runtime_error("evaluate: empty expression");

    // Set scope to the requested frame. frameId == 0 means "global"
    // (no frame context); we leave the engine's current scope alone.
    if (frameId != 0) {
        auto fit = frames_.find(frameId);
        if (fit == frames_.end())
            throw std::runtime_error("evaluate: stale frameId");
        if (system_)
            system_->SetCurrentThreadId(fit->second.threadEngineId);
        symbols_->SetScopeFrameByIndex(fit->second.frameIndex);
    }

    const std::wstring exprW = to_wide(expression);

    // Single path: build a synthetic IDebugSymbolGroup2 with the
    // expression as its sole entry. AddSymbolWide accepts both bare
    // identifiers and C-like expressions, and the engine renders the
    // result the same way it renders Locals — including NatVis on
    // compound types and proper dereferencing of local symbols.
    //
    // (We deliberately skip IDebugControl::EvaluateWide here. It works
    // for arithmetic on register/literal expressions, but for a local
    // identifier like `x` it returns the *address* of x, not its value
    // — the symbol-group renderer is the one that resolves to the
    // current contents.)
    IDebugSymbolGroup2* group = nullptr;
    HRESULT hrCreate = symbols_->CreateSymbolGroup2(&group);
    if (FAILED(hrCreate) || !group)
        throw hr_error(hrCreate, "evaluate: CreateSymbolGroup2");
    ULONG idx = DEBUG_ANY_ID;
    HRESULT hrAdd = group->AddSymbolWide(
        const_cast<PWSTR>(exprW.c_str()), &idx);
    if (FAILED(hrAdd)) {
        group->Release();
        throw hr_error(hrAdd, "evaluate: AddSymbolWide");
    }

    DEBUG_SYMBOL_PARAMETERS pp = {};
    group->GetSymbolParameters(idx, 1, &pp);

    wchar_t valBuf[1024] = {};
    ULONG valSize = 0;
    if (SUCCEEDED(group->GetSymbolValueTextWide(idx, valBuf, 1024, &valSize))
        && valSize > 1) {
        r.result = to_utf8(valBuf);
    }
    wchar_t typeBuf[256] = {};
    ULONG typeSize = 0;
    if (SUCCEEDED(group->GetSymbolTypeNameWide(idx, typeBuf, 256, &typeSize))
        && typeSize > 1) {
        r.type = to_utf8(typeBuf);
    }

    if (pp.SubElements > 0) {
        const int newRef = nextVarRef_++;
        synthGroups_[newRef] = group;  // owned; freed on next stop
        // frameId=0 sentinel routes getVariables/setVariable to synthGroups_.
        varRefs_[newRef] = VarRefBinding{0, idx};
        r.variablesReference = newRef;
    } else {
        group->Release();
    }
    return r;
}

// ---- Event hooks invoked by EventCallbacks on the engine thread ----

void Engine::onProcessCreated() {
    if (!session_) return;
    dap::ProcessEvent event;
    event.name = "debuggee";
    event.startMethod = "launch";
    session_->send(event);
}

void Engine::onProcessExited(ULONG exitCode) {
    targetRunning_ = false;
    processLive_ = false;
    if (!session_) return;
    dap::ExitedEvent exited;
    exited.exitCode = static_cast<dap::integer>(exitCode);
    session_->send(exited);
    dap::TerminatedEvent terminated;
    session_->send(terminated);
}

void Engine::onBreakpoint(ULONG id) {
    targetRunning_ = false;
    if (entryPending_ && id == entryBreakpointId_) {
        entryPending_ = false;
        entryStopPending_ = true;
        return;
    }
    if (id == stepOutBpId_ && stepOutBpId_ != 0) {
        stepOutBpId_ = 0;
        emitStopped(StopReason::Step, {});
        return;
    }
    if (pauseRequested_) {
        pauseRequested_ = false;
        emitStopped(StopReason::Pause, {});
        return;
    }

    // Condition / log handling. Evaluating a C-style expression against
    // local variables requires the symbol-group renderer (EvaluateWide
    // returns the *address* of locals, not their values). Build a
    // throwaway one-symbol group with the condition expression and read
    // it back as text — non-zero text result counts as "true".
    if (auto it = bpAttrs_.find(id); it != bpAttrs_.end()) {
        const BpAttrs& a = it->second;
        bool truthy = true;
        if (!a.condition.empty()) {
            truthy = false;
            IDebugSymbolGroup2* group = nullptr;
            if (SUCCEEDED(symbols_->CreateSymbolGroup2(&group)) && group) {
                ULONG idx = DEBUG_ANY_ID;
                std::wstring exprW = to_wide(a.condition);
                if (SUCCEEDED(group->AddSymbolWide(
                        const_cast<PWSTR>(exprW.c_str()), &idx))) {
                    wchar_t buf[64] = {};
                    ULONG sz = 0;
                    if (SUCCEEDED(group->GetSymbolValueTextWide(
                            idx, buf, 64, &sz)) && sz > 1) {
                        std::string s = to_utf8(buf);
                        // Truthy iff result is a non-zero number.
                        // DbgEng renders "0n0" / "0x0" / "0" for zero.
                        if (s != "0n0" && s != "0x0" && s != "0"
                            && s != "false") {
                            truthy = true;
                        }
                    }
                }
                group->Release();
            }
        }
        if (!truthy) {
            // Condition false — auto-resume from this BP without
            // surfacing a stopped event to the client.
            callbackResume_ = true;
            targetRunning_ = true;
            return;
        }
        if (!a.logMessage.empty()) {
            if (session_) {
                dap::OutputEvent ev;
                ev.category = "console";
                ev.output = a.logMessage + "\n";
                session_->send(ev);
            }
            callbackResume_ = true;
            targetRunning_ = true;
            return;
        }
    }
    emitStopped(StopReason::Breakpoint, {});
}

void Engine::onException(PEXCEPTION_RECORD64 ex, bool firstChance) {
    targetRunning_ = false;
    // STATUS_BREAKPOINT is the initial loader break on process create; it
    // is expected and handled inside launch(), so we treat it as a
    // routine break rather than a user-visible exception.
    if (ex && ex->ExceptionCode == 0x80000003 /* STATUS_BREAKPOINT */ &&
        entryPending_) {
        return;
    }
    // SetInterrupt typically lands here as STATUS_BREAKPOINT once the
    // engine injects its int3 to halt the target.
    if (pauseRequested_) {
        pauseRequested_ = false;
        emitStopped(StopReason::Pause, {});
        return;
    }
    // Single-step completion from STEP_OVER / STEP_INTO arrives as a
    // STATUS_SINGLE_STEP exception.
    if (ex && ex->ExceptionCode == 0x80000004 /* STATUS_SINGLE_STEP */) {
        emitStopped(StopReason::Step, {});
        return;
    }

    // First-chance exceptions are normally caught by the target's own
    // handlers — we only break if the user asked for them via the
    // exception filter. Second-chance (unhandled) exceptions always
    // break: they're crashes about to terminate the process.
    if (firstChance && !firstChanceBreak_) {
        callbackResume_ = true;
        targetRunning_ = true;
        return;
    }

    std::string desc = firstChance ? "first-chance exception"
                                   : "second-chance exception";
    emitStopped(StopReason::Exception, desc);
}

void Engine::setExceptionBreakpoints(
    const std::vector<std::string>& filters) {
    firstChanceBreak_ = false;
    for (const std::string& f : filters) {
        if (f == "firstChance") firstChanceBreak_ = true;
    }
}

void Engine::onSessionStatus(ULONG /*status*/) {
    // M2+ will surface status changes as DAP output events.
}

}  // namespace cppdbg
