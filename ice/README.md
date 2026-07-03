# Cross-machine time service with ZeroC Ice

An ORB-style RPC across the network **without HTTP**: the Ubuntu box hosts a
`TimeSvc` object, the Windows box calls `getLocalTime()` on it over Ice's
compact binary protocol on plain TCP.

- IDL: `Time.ice` (Slice) — shared by both ends.
- Server: `server.cpp` → `timeserver` on Ubuntu (listens on TCP 10000).
- Client: `client.cpp` → `client.exe` on Windows (connects to `UBUNTU_HOST:10000`).

`slice2cpp` generates `Time.h` + `Time.cpp` from `Time.ice`; both the server and
the client compile that generated `Time.cpp` alongside their own source.

## Ubuntu — server

Install Ice (ZeroC apt repo; the `-all-dev` package includes `slice2cpp` and the
C++ headers/libs):

```bash
sudo apt update
sudo apt install zeroc-ice-all-dev        # slice2cpp + libIce++11 + headers
```

Generate the stub and build:

```bash
cd ice
slice2cpp Time.ice
g++ -std=c++17 server.cpp Time.cpp -lIce++11 -lpthread -o timeserver
./timeserver
```

Open the port so the Windows client can reach it:

```bash
sudo ufw allow 10000/tcp
```

> Link note: Ice 3.7 ships two C++ mappings — link **`-lIce++11`** for the
> modern (C++11) mapping this code uses, not plain `-lIce` (that's the C++98
> mapping and won't match these signatures).

## Windows — client

1. Install Ice: either the **ZeroC Ice MSI** (gives you `slice2cpp.exe` +
   SDK), or add the **ZeroC Ice NuGet package** matching your VS platform
   toolset to a C++ project. The NuGet package sets the include/lib paths and
   copies the runtime DLLs.
2. Generate the stub (Developer Command Prompt):
   ```bat
   slice2cpp Time.ice
   ```
3. Build a console app from `client.cpp` + generated `Time.cpp`, linking the Ice
   C++11 import lib (e.g. `Ice++11.lib` / `ice37++11.lib`, name depends on the
   Ice version). With the NuGet package this is automatic.
4. Edit `client.cpp`: replace `UBUNTU_HOST` with the server's hostname or IP.

Run the server first, then the client:

```bat
client.exe
:: -> Remote local time: 14:22:07
```

## Why this fits "ORB, not HTTP"

- **Object model**: `TimeSvc` is a remote *object*; `Demo::TimeSvcPrx` is a
  local proxy standing in for it. Calls look like ordinary method calls — the
  ORB (the `Ice::Communicator`) marshals them and ships them to the servant.
- **Transport**: Ice's own binary encoding over **TCP** (`tcp -p 10000`). No
  HTTP, no REST, no JSON. (Ice can also use SSL, WebSocket, or UDP, but the
  default here is raw TCP.)
- **No naming service needed**: the proxy string carries the endpoint, so
  there's nothing extra to run. For larger systems Ice has **IceGrid**
  (locator/registry), the analog of a CORBA Naming Service.

## Folding it into WinApiDemo (optional)

The Windows `WndProc` clock could call the Ice proxy instead of MS-RPC: hold a
`Demo::TimeSvcPrx` (bound once, like the RPC binding), and in the 1 Hz
`WM_TIMER` do `lstrcpynW(g_time, ..., svc->getLocalTime())` inside a try/catch,
falling back to "FAIL". That turns the local same-machine RPC clock into a
cross-machine one, pointed at the Ubuntu server.
