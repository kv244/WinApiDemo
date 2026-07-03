// time.c - RPC *server* for the Timesvc interface.
// Hosts GetLocalTimeString on the local ncalrpc transport (ALPC underneath)
// at endpoint "WinApiDemoTime". Run this first; then start WinApiDemo, which
// connects as the client at startup.
//
// Build (command line):
//   midl Timesvc.idl
//   cl /nologo time.c Timesvc_s.c
// (rpcrt4.lib is pulled in by the #pragma below.)

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "Timesvc.h"        // MIDL-generated header

#pragma comment(lib, "rpcrt4.lib")
#pragma comment(lib, "user32.lib")   // wsprintfW lives here

// --- The remotely-callable procedure. Signature must match Timesvc.h. ---
long GetLocalTimeString(wchar_t szTime[64])
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    wsprintfW(szTime, L"%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);
    return 0;   // 0 = success
}

// RPC requires these allocators for any marshaling that needs heap memory.
void* __RPC_USER midl_user_allocate(size_t size) { return malloc(size); }
void  __RPC_USER midl_user_free(void* p)         { free(p); }

int wmain(void)
{
    RPC_STATUS s;

    // 1. Register the local endpoint (protocol sequence + endpoint name).
    s = RpcServerUseProtseqEpW((RPC_WSTR)L"ncalrpc",
                               RPC_C_PROTSEQ_MAX_REQS_DEFAULT,
                               (RPC_WSTR)L"WinApiDemoTime",
                               NULL);
    if (s != RPC_S_OK) { wprintf(L"UseProtseqEp failed: %ld\n", s); return 1; }

    // 2. Register the interface so calls are dispatched to our functions.
    s = RpcServerRegisterIf(Timesvc_v1_0_s_ifspec, NULL, NULL);
    if (s != RPC_S_OK) { wprintf(L"RegisterIf failed: %ld\n", s); return 1; }

    wprintf(L"Time RPC server listening on ncalrpc:WinApiDemoTime.\n"
            L"Leave this running, start WinApiDemo, then Ctrl+C to stop.\n");

    // 3. Block, servicing calls until the process is killed.
    s = RpcServerListen(1, RPC_C_LISTEN_MAX_CALLS_DEFAULT, FALSE);
    if (s != RPC_S_OK) { wprintf(L"ServerListen failed: %ld\n", s); return 1; }

    return 0;
}
