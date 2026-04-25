#pragma once

#include <windows.h>
#include <dbgeng.h>

#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "command_queue.h"
#include "event_callbacks.h"

namespace dap {
class Session;
}

namespace cppdbg {

// Reason reported on a `stopped` event.
enum class StopReason { Entry, Breakpoint, Exception, Step, Pause };

// Per-line input to setSourceBreakpoints.
struct SourceBreakpointSpec {
    int line = 0;
    std::string condition;     // DbgEng-syntax expression; empty = no condition
    std::string hitCondition;  // simple integer N: fire on the Nth hit
    std::string logMessage;    // non-empty = log+continue instead of stopping
};

// Per-breakpoint resolution result returned from setSourceBreakpoints
// and setFunctionBreakpoints.
struct SourceBreakpoint {
    int requestedLine = 0;
    int actualLine = 0;   // line PDB resolved to; same as requestedLine if exact
    bool verified = false;
    ULONG engineId = 0;   // DbgEng breakpoint id (0 if unverified)
};

// Function-breakpoint input.
struct FunctionBreakpointSpec {
    std::string name;          // symbol name; e.g. "hello!main" or just "main"
    std::string condition;
    std::string hitCondition;
};

// ---- M3 inspection types ----------------------------------------------

struct DapThread {
    int id;            // DbgEng-local thread id, surfaced as DAP threadId
    std::string name;
};

struct DapFrame {
    int id;            // unique within current pause
    std::string name;
    std::string sourcePath;  // empty when no PDB info
    int line = 0;
    ULONG64 instructionOffset = 0;
};

struct DapScope {
    std::string name;
    std::string presentationHint;  // "locals", "arguments", ...
    int variablesReference = 0;
    bool expensive = false;
};

struct DapVariable {
    std::string name;
    std::string value;
    std::string type;
    int variablesReference = 0;  // 0 = leaf, no children
};

struct DapEvaluateResult {
    std::string result;
    std::string type;
    int variablesReference = 0;  // > 0 if compound — drill via getVariables
};

// Owns every DbgEng COM interface and the thread they run on. The DAP
// session talks to the engine only through CommandQueue::post, so every
// COM call lives on the same engine thread (MTA apartment — DbgEng's
// WaitForEvent does not pump messages, so STA would deadlock).
class Engine {
public:
    Engine();
    ~Engine();

    // DAP session the engine pushes events onto. Set before start().
    void setSession(dap::Session* session) { session_ = session; }

    void start();  // spawns engine thread
    void stop();   // asks engine thread to exit and joins

    CommandQueue& commands() { return queue_; }

    // ---- Called from the engine thread (command closures) ----
    void launch(const std::wstring& program, const std::wstring& args,
                const std::wstring& cwd);
    // Attach to an already-running process by OS PID. Halts the target
    // (DbgEng injects a breakpoint thread) and defers stopped(entry) to
    // configurationDone, mirroring the launch flow.
    void attach(ULONG processId);
    void resume();
    void detachOrTerminate(bool terminate);

    // Stepping. Each switches the engine to the given thread first, then
    // sets the appropriate execution status. Returns immediately — the
    // step completes asynchronously and surfaces a stopped(step) event.
    void stepOver(int threadEngineId);
    void stepInto(int threadEngineId);
    void stepOut(int threadEngineId);
    // Asynchronously interrupt a running target. Surfaces stopped(pause).
    void pause();

    // Replace all breakpoints for `sourcePath` with ones at the given
    // specs. Unresolvable specs come back with verified=false.
    std::vector<SourceBreakpoint> setSourceBreakpoints(
        const std::string& sourcePath,
        const std::vector<SourceBreakpointSpec>& specs);
    // Replace all function breakpoints with the given set.
    std::vector<SourceBreakpoint> setFunctionBreakpoints(
        const std::vector<FunctionBreakpointSpec>& specs);
    // Decode `count` instructions starting at `address`. resolveSymbols
    // controls whether DbgEng prefixes module!sym annotations.
    struct DisassembledInstruction {
        std::string address;     // "0x..." hex string
        std::string instruction; // operands too; matches DbgEng output
        std::string symbol;      // module!name+offset, if resolvable
    };
    std::vector<DisassembledInstruction> disassemble(ULONG64 address,
                                                     int count,
                                                     bool resolveSymbols);

    // Update the active exception filters. Currently we expose one
    // toggle: "firstChance" — when present, every first-chance exception
    // breaks; when absent, first-chance exceptions are silently passed
    // back to the target. Second-chance exceptions always break.
    void setExceptionBreakpoints(const std::vector<std::string>& filters);

    // Inspection — only valid while the target is paused. IDs returned
    // here are invalidated on the next stopped event.
    std::vector<DapThread> getThreads();
    std::vector<DapFrame> getStackTrace(int threadEngineId, int startFrame,
                                        int levels);
    std::vector<DapScope> getScopes(int frameId);
    std::vector<DapVariable> getVariables(int variablesReference);
    // Write a new value into the named variable inside the container
    // identified by variablesReference. Returns the value as the engine
    // formatted it after the assignment (canonical form).
    DapVariable setVariable(int variablesReference, const std::string& name,
                            const std::string& value);

    // Evaluate a free-form expression in the scope of `frameId` (or
    // global scope if frameId == 0). Tier 1 uses EvaluateWide for
    // primitives; tier 2 builds a synthetic IDebugSymbolGroup2 so
    // compound results plug back into the variables/setVariable
    // machinery via the returned variablesReference.
    DapEvaluateResult evaluate(int frameId, const std::string& expression,
                               const std::string& context);

    // ---- Called from event callbacks (engine thread) ----
    void onProcessCreated();
    void onProcessExited(ULONG exitCode);
    void onBreakpoint(ULONG id);
    void onException(PEXCEPTION_RECORD64 ex, bool firstChance);
    void onSessionStatus(ULONG status);
    // Read+clear the one-shot resume flag set by onBreakpoint when a
    // conditional/log BP wants to be transparent. event_callbacks uses
    // this to choose DEBUG_STATUS_GO vs DEBUG_STATUS_BREAK.
    bool consumeCallbackResume() {
        bool v = callbackResume_;
        callbackResume_ = false;
        return v;
    }

private:
    void threadMain();
    void initCom();
    void shutdownCom();
    void waitForNextEvent();
    ULONG currentThreadId();
    void emitStopped(StopReason reason, const std::string& description);
    // Drop frame + variablesReference caches. Called whenever the target
    // resumes or hits a new break — the DAP spec invalidates these IDs
    // every time execution proceeds.
    void invalidateInspectionCaches();
    // Resolve the IDebugSymbolGroup2 backing a varRef. Routes to
    // synthGroups_ (evaluate's synthetic groups, frameId == 0) or to
    // frameGroups_ (locals, lazily built per frame). Returns nullptr
    // for stale refs / build failures. Non-owning pointer; lifetime
    // tied to invalidateInspectionCaches.
    IDebugSymbolGroup2* groupForVarRef(int varRef);

    dap::Session* session_ = nullptr;

    std::thread thread_;
    std::atomic<bool> running_{false};

    CommandQueue queue_;
    EventCallbacks callbacks_{this};

    // COM interfaces (all live on the engine thread).
    IDebugClient5* client_ = nullptr;
    IDebugControl5* control_ = nullptr;  // 5+ for GetStackTraceEx
    IDebugSymbols3* symbols_ = nullptr;
    IDebugSystemObjects* system_ = nullptr;
    IDebugRegisters2* registers_ = nullptr;

    // Engine state.
    bool processLive_ = false;      // debuggee exists
    bool targetRunning_ = false;    // WaitForEvent should be called
    bool entryPending_ = true;      // next hit should be reported as entry
    bool entryStopPending_ = false; // entry hit but stopped event deferred
                                    // until configurationDone surfaces it
    ULONG entryBreakpointId_ = 0;   // id of the $exentry bp
    ULONG stepOutBpId_ = 0;         // one-shot bp at caller for stepOut

    // Per-BP-id condition/log metadata. We evaluate these inside the
    // Breakpoint callback and return DEBUG_STATUS_GO to skip the stop
    // when a condition is false or after we've handled a logpoint —
    // SetCommandWide alone runs *after* the callback, so the user still
    // sees a transient stopped event.
    struct BpAttrs {
        std::string condition;
        std::string logMessage;
    };
    std::unordered_map<ULONG, BpAttrs> bpAttrs_;
    // One-shot flag set by onBreakpoint / onException when the callback
    // should return GO instead of BREAK. Read+cleared by event_callbacks.
    bool callbackResume_ = false;
    // Exception filtering — see setExceptionBreakpoints.
    bool firstChanceBreak_ = false;
    // STEP_INTO/STEP_OVER are *instruction-level* in DbgEng and complete
    // silently — the engine transitions back to BREAK with no callback.
    // To deliver source-line semantics expected by DAP, we anchor the
    // step to the starting line+function and keep instruction-stepping
    // until either the line or the frame's function changes.
    enum class StepKind { None, Over, Into };
    StepKind stepKind_ = StepKind::None;
    ULONG stepStartLine_ = 0;
    std::wstring stepStartFunc_;
    // pause() is the only state-mutating call the DAP thread makes
    // directly (SetInterrupt is documented as thread-safe), so this
    // flag must be atomic — the engine thread reads it inside callbacks.
    std::atomic<bool> pauseRequested_{false};

    // source path → engine BP ids currently set for that source. Used
    // to remove stale BPs before installing a new set.
    std::unordered_map<std::string, std::vector<ULONG>> sourceBpIds_;
    // engine ids of currently-installed function breakpoints; cleared
    // and reinstalled on every setFunctionBreakpoints call.
    std::vector<ULONG> funcBpIds_;

    // ---- Inspection caches; cleared on every stopped event ----
    struct CachedFrame {
        ULONG threadEngineId = 0;
        ULONG frameIndex = 0;       // 0 = innermost
        ULONG64 instructionOffset = 0;
    };
    struct VarRefBinding {
        int frameId = 0;
        ULONG parentIdx = ~0u;      // ~0u → top-level "Locals" scope
    };
    std::unordered_map<int, CachedFrame> frames_;
    std::unordered_map<int, VarRefBinding> varRefs_;
    std::unordered_map<int, IDebugSymbolGroup2*> frameGroups_;  // owning
    // Synthetic groups created by evaluate(); keyed by the same
    // variablesReference. Distinguished from frame groups by
    // VarRefBinding.frameId == 0.
    std::unordered_map<int, IDebugSymbolGroup2*> synthGroups_;  // owning
    int nextFrameId_ = 1;
    int nextVarRef_ = 1;
};

}  // namespace cppdbg
