#include "event_callbacks.h"

#include "engine.h"

namespace cppdbg {

STDMETHODIMP EventCallbacks::QueryInterface(REFIID iid, void** iface) {
    if (!iface) return E_POINTER;
    if (iid == __uuidof(IUnknown) ||
        iid == __uuidof(IDebugEventCallbacksWide)) {
        *iface = static_cast<IDebugEventCallbacksWide*>(this);
        return S_OK;
    }
    *iface = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP EventCallbacks::GetInterestMask(PULONG mask) {
    *mask = DEBUG_EVENT_BREAKPOINT | DEBUG_EVENT_EXCEPTION |
            DEBUG_EVENT_CREATE_THREAD | DEBUG_EVENT_EXIT_THREAD |
            DEBUG_EVENT_CREATE_PROCESS | DEBUG_EVENT_EXIT_PROCESS |
            DEBUG_EVENT_LOAD_MODULE | DEBUG_EVENT_UNLOAD_MODULE |
            DEBUG_EVENT_SYSTEM_ERROR | DEBUG_EVENT_SESSION_STATUS |
            DEBUG_EVENT_CHANGE_ENGINE_STATE;
    return S_OK;
}

STDMETHODIMP EventCallbacks::Breakpoint(PDEBUG_BREAKPOINT2 bp) {
    ULONG id = 0;
    if (bp) bp->GetId(&id);
    engine_->onBreakpoint(id);
    // Conditional/log BPs ask us to resume transparently — return GO so
    // the engine continues without surfacing the break.
    return engine_->consumeCallbackResume() ? DEBUG_STATUS_GO
                                            : DEBUG_STATUS_BREAK;
}

STDMETHODIMP EventCallbacks::Exception(PEXCEPTION_RECORD64 ex,
                                       ULONG firstChance) {
    engine_->onException(ex, firstChance != 0);
    // First-chance exceptions outside our active filter set ask us to
    // pass them back to the target's handler chain — same callback-resume
    // mechanism conditional BPs use.
    return engine_->consumeCallbackResume() ? DEBUG_STATUS_GO
                                            : DEBUG_STATUS_BREAK;
}

STDMETHODIMP EventCallbacks::CreateThread(ULONG64, ULONG64, ULONG64) {
    return DEBUG_STATUS_NO_CHANGE;
}

STDMETHODIMP EventCallbacks::ExitThread(ULONG) {
    return DEBUG_STATUS_NO_CHANGE;
}

STDMETHODIMP EventCallbacks::CreateProcessW(
    ULONG64, ULONG64, ULONG64, ULONG, PCWSTR, PCWSTR, ULONG, ULONG,
    ULONG64, ULONG64, ULONG64) {
    engine_->onProcessCreated();
    return DEBUG_STATUS_NO_CHANGE;
}

STDMETHODIMP EventCallbacks::ExitProcess(ULONG exitCode) {
    engine_->onProcessExited(exitCode);
    return DEBUG_STATUS_NO_CHANGE;
}

STDMETHODIMP EventCallbacks::LoadModule(ULONG64, ULONG64, ULONG, PCWSTR,
                                        PCWSTR, ULONG, ULONG) {
    return DEBUG_STATUS_NO_CHANGE;
}

STDMETHODIMP EventCallbacks::UnloadModule(PCWSTR, ULONG64) {
    return DEBUG_STATUS_NO_CHANGE;
}

STDMETHODIMP EventCallbacks::SystemError(ULONG, ULONG) {
    return DEBUG_STATUS_NO_CHANGE;
}

STDMETHODIMP EventCallbacks::SessionStatus(ULONG status) {
    engine_->onSessionStatus(status);
    return S_OK;
}

STDMETHODIMP EventCallbacks::ChangeDebuggeeState(ULONG, ULONG64) {
    return S_OK;
}

STDMETHODIMP EventCallbacks::ChangeEngineState(ULONG, ULONG64) {
    return S_OK;
}

STDMETHODIMP EventCallbacks::ChangeSymbolState(ULONG, ULONG64) {
    return S_OK;
}

}  // namespace cppdbg
