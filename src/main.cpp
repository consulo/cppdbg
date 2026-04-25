#include <windows.h>
#include <fcntl.h>
#include <io.h>

#include <dap/io.h>
#include <dap/protocol.h>
#include <dap/session.h>

#include <cstdio>
#include <future>

#include "engine.h"
#include "handlers/breakpoints.h"
#include "handlers/inspection.h"
#include "handlers/lifecycle.h"
#include "handlers/stepping.h"

int main() {
    // DAP speaks binary framing on stdio; force binary mode so Windows
    // does not translate LF<->CRLF.
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);

    auto session = dap::Session::create();
    cppdbg::Engine engine;
    engine.setSession(session.get());

    std::promise<void> disconnected;
    auto disconnectedFuture = disconnected.get_future();
    cppdbg::registerLifecycleHandlers(*session, engine, disconnected);
    cppdbg::registerBreakpointHandlers(*session, engine);
    cppdbg::registerInspectionHandlers(*session, engine);
    cppdbg::registerSteppingHandlers(*session, engine);

    session->onError([&](const char* msg) {
        dap::OutputEvent out;
        out.category = "stderr";
        out.output = std::string("dap: ") + msg + "\n";
        session->send(out);
    });

    engine.start();
    auto in = dap::file(stdin, false);
    auto out = dap::file(stdout, false);
    session->bind(in, out);

    disconnectedFuture.wait();
    engine.stop();
    // cppdap's session reader is blocked in a Windows ReadFile on stdin;
    // closing the peer end doesn't always return EOF. Skip the destructor
    // chain and exit directly — everything we care about is already flushed.
    std::fflush(stdout);
    std::fflush(stderr);
    std::_Exit(0);
}
