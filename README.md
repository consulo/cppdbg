# cppdbg

Native Windows C++ Debug Adapter Protocol (DAP) server for debugging
MSVC-built executables. Drives the Microsoft **DbgEng** API under the
hood and exposes DAP over stdio, so any DAP client (Consulo, VS Code,
Neovim DAP, Emacs `dape`, ...) can drive it.

## Status

**All eight milestones complete.** Launch *or* attach by PID, source +
function breakpoints with conditions / hit counts / log messages,
threads + call stack + scopes + locals, source-line step over / into /
out / pause, `setVariable`, `evaluate` (REPL / watch / hover, with
NatVis if your PDB ships it), exception filters, and `disassemble`.

## Why native C++

- DbgEng is COM. In C++ it's vtable calls; in other languages it's
  P/Invoke, STA apartment plumbing, and callback marshalling.
- In-process avoids serialisation tax on module-load / breakpoint-hit
  events during startup (easily thousands per session).
- Single `.exe` linked against `dbgeng.dll` (already on every Windows
  install). Zero runtime bundle.

## Building

Requires Windows, MSVC 19.38+ (or clang-cl), Windows 10 SDK, CMake 3.24+.
Both **x86_64** and **aarch64** are supported.

```sh
git clone --recurse-submodules https://github.com/consulo/cppdbg.git
cd cppdbg
# Pick one:
cmake -B build -S . -A x64           # x86_64 host
cmake -B build -S . -A ARM64         # arm64 host (or cross-build)
cmake --build build --config Release
# Produces build/Release/cppdbg.exe
```

CI builds both architectures on every push and publishes the resulting
binaries to [`consulo/binaries`](https://github.com/consulo/binaries)
under `windows-x86_64/` and `windows-aarch64/`.

## Running

`cppdbg.exe` speaks DAP on stdio. A minimal `launch.json` for a generic
DAP client:

```json
{
  "type": "cppdbg",
  "request": "launch",
  "program": "C:\\path\\to\\your\\app.exe",
  "args": [],
  "cwd": "C:\\path\\to"
}
```

## Licence

Apache-2.0. Matches [`google/cppdap`](https://github.com/google/cppdap).
