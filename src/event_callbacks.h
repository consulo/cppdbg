#pragma once

#include <windows.h>
#include <dbgeng.h>

namespace cppdbg {

class Engine;

// Implements only the subset of events M1 needs. Everything else returns
// DEBUG_STATUS_NO_CHANGE (let the engine decide what to do next).
class EventCallbacks : public IDebugEventCallbacksWide {
public:
    explicit EventCallbacks(Engine* engine) : engine_(engine) {}

    // IUnknown
    STDMETHOD(QueryInterface)(REFIID iid, void** iface) override;
    STDMETHOD_(ULONG, AddRef)() override { return 1; }   // stack-owned
    STDMETHOD_(ULONG, Release)() override { return 1; }

    // IDebugEventCallbacksWide
    STDMETHOD(GetInterestMask)(PULONG mask) override;

    STDMETHOD(Breakpoint)(PDEBUG_BREAKPOINT2 bp) override;
    STDMETHOD(Exception)(PEXCEPTION_RECORD64 ex, ULONG firstChance) override;
    STDMETHOD(CreateThread)(ULONG64 handle, ULONG64 dataOffset,
                            ULONG64 startOffset) override;
    STDMETHOD(ExitThread)(ULONG exitCode) override;
    STDMETHOD(CreateProcessW)(ULONG64 imageFileHandle, ULONG64 handle,
                              ULONG64 baseOffset, ULONG moduleSize,
                              PCWSTR moduleName, PCWSTR imageName,
                              ULONG checkSum, ULONG timeDateStamp,
                              ULONG64 initialThreadHandle,
                              ULONG64 threadDataOffset,
                              ULONG64 startOffset) override;
    STDMETHOD(ExitProcess)(ULONG exitCode) override;
    STDMETHOD(LoadModule)(ULONG64 imageFileHandle, ULONG64 baseOffset,
                          ULONG moduleSize, PCWSTR moduleName,
                          PCWSTR imageName, ULONG checkSum,
                          ULONG timeDateStamp) override;
    STDMETHOD(UnloadModule)(PCWSTR imageBaseName, ULONG64 baseOffset) override;
    STDMETHOD(SystemError)(ULONG error, ULONG level) override;
    STDMETHOD(SessionStatus)(ULONG status) override;
    STDMETHOD(ChangeDebuggeeState)(ULONG flags, ULONG64 argument) override;
    STDMETHOD(ChangeEngineState)(ULONG flags, ULONG64 argument) override;
    STDMETHOD(ChangeSymbolState)(ULONG flags, ULONG64 argument) override;

private:
    Engine* engine_;
};

}  // namespace cppdbg
