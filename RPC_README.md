# WinApiDemo RPC time service

WinApiDemo asks a separate process for the local time over **MS-RPC** and shows
it in the top-right corner, refreshed **once per second**. If the server isn't
running (or anything else fails), it shows **FAIL** instead — and recovers
automatically once the server comes up.

- Transport: `ncalrpc` (local RPC, ALPC underneath — single machine only).
- Endpoint name: `WinApiDemoTime`.
- Interface: `Timesvc` v1.0, one method `GetLocalTimeString`.

## Files

| File             | Role                                                     |
|------------------|----------------------------------------------------------|
| `Timesvc.idl`    | Interface definition (compiled by MIDL).                 |
| `Timesvc.acf`    | Uses an implicit binding handle (`Timesvc_Binding`).     |
| `time.c`         | The RPC **server** — its own console `.exe`.             |
| `WinApiDemo.cpp` | The RPC **client** (plus everything else).               |
| `CMakeLists.txt` | Builds both executables + the MIDL step.                 |

MIDL turns `Timesvc.idl` + `Timesvc.acf` into three generated files:

- `Timesvc.h`   — shared header (both sides `#include` it)
- `Timesvc_c.c` — **client** stub (link into WinApiDemo)
- `Timesvc_s.c` — **server** stub (link into TimeServer)

> **The `_c` and `_s` stubs must never be linked into the same binary.** The
> client stub and the server implementation both define `GetLocalTimeString`;
> together they collide (LNK2005). Client and server are separate executables.

## Option A — CMake (recommended)

Structurally enforces the two-executable split. Run from a **Developer Command
Prompt** (or a VSCode CMake Tools kit like "Visual Studio … amd64") so `cl` and
`midl` are on `PATH`.

```bat
cmake -S . -B build -A x64          :: or: -A Win32
cmake --build build --config Debug
```

Outputs land in `build\Debug\`. Run the server first:

```bat
cd build\Debug
start TimeServer.exe                 :: leave running
WinApiDemo.exe
```

The `-A` flag is for the Visual Studio generator on the command line; with the
VSCode CMake Tools extension you select the architecture via the kit instead.

> `capture.wmf` (ESC) is written to the process working directory — for the
> CMake build that's `build\Debug\`.

## Option B — command line (Developer Command Prompt)

```bat
cd C:\Users\julia\source\repos\WinApiDemo

:: 1. Generate the stubs from the interface
midl Timesvc.idl

:: 2. Build the server (console app)
cl /nologo time.c Timesvc_s.c

:: 3. Build the client (windowed app) — client stub + /SUBSYSTEM:WINDOWS
cl /nologo /EHsc WinApiDemo.cpp Timesvc_c.c /link /SUBSYSTEM:WINDOWS

:: 4. Run: server first, then client
start time.exe
WinApiDemo.exe
```

The import libs (`rpcrt4`, `user32`, `gdi32`) are pulled in by
`#pragma comment(lib, ...)` in the sources, so they don't need to be named on
the `cl` line. Kill the server (Ctrl+C) and watch the clock flip to **FAIL**;
restart it and it recovers within a second.

## Option C — Visual Studio (two projects in one solution)

**Client project (existing WinApiDemo):**
1. Add `Timesvc.idl` to the project — VS runs MIDL automatically on build.
2. Add the generated `Timesvc_c.c` to the project.
3. Do **not** add `time.c` or `Timesvc_s.c` here (that causes the LNK2005
   collision above).

**Server project (new):**
1. Add a new **Console App (C++)** project, e.g. `TimeServer`.
2. Add `time.c` + `Timesvc.idl`, then the generated `Timesvc_s.c`.
3. Run it before launching WinApiDemo.

> Tip: the client compiles even before the stub exists — the code guards the
> RPC path with `__has_include("Timesvc.h")`, so without it the clock just shows
> FAIL. Add the header to the include path and link `Timesvc_c.c` to turn it on.

## Changing the interface UUID

The interface GUID lives in `Timesvc.idl` (`uuid(...)`). Client and server must
share the same one. If you change it, **re-run MIDL** so both stubs pick it up
(CMake and VS do this automatically on rebuild; the command-line path needs a
fresh `midl Timesvc.idl`). Generate your own with `uuidgen -c` or VS's
**Tools → Create GUID** (Registry Format).

## How it works (quick tour)

- **Server** (`time.c`): `RpcServerUseProtseqEp` registers the local endpoint,
  `RpcServerRegisterIf` wires the interface to the implementation, and
  `RpcServerListen` blocks servicing calls.
- **Client** (`WinApiDemo.cpp`): `EnsureBinding` builds the binding to
  `ncalrpc:WinApiDemoTime` **once** (binding is lazy — it doesn't connect until
  the first call). A 1 Hz `WM_TIMER` calls `UpdateTimeViaRpc`, which makes the
  call inside `RpcTryExcept/RpcExcept` because transport/server errors surface
  as exceptions, not return codes. The binding is freed in `WM_DESTROY`.
- The implicit handle (`Timesvc_Binding`, from the `.acf`) keeps the client and
  server function signatures identical — no `handle_t` argument to juggle.
```
