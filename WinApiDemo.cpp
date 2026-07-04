// WinApiDemo.cpp - Win32 window with a WASD-controlled sprite over a randomly
// generated ragged-mountain background (cyan sky, yellow ground).
//
// - Diagonal moves (W+D, W+A, S+A, S+D) are speed-normalized, so the sprite
//   travels the same pixels/frame in every direction.
// - The scene is double buffered: background + sprite are drawn to an
//   off-screen bitmap and blitted once per frame -> no flicker, no trails.
// - The mountain is generated once (1D midpoint displacement) and cached;
//   it is only regenerated when the window size changes.
//
// No .rc file, no resource headers, no libraries beyond the Win32 API + CRT.
// Build in VS 2026: Empty Project -> add this file ->
//   Linker > System > SubSystem = Windows (/SUBSYSTEM:WINDOWS)

#include <windows.h>
#include <stdlib.h>     // rand, srand
#include <math.h>       // sqrt
#include <emmintrin.h>  // SSE2 intrinsics (x64 clamp path)

// Windows import libs. A VS "Desktop" project links these by default, but a
// bare command-line cl build does not -- so pin them to the source.
#pragma comment(lib, "user32.lib")   // window, menu, clipboard, messages, timers
#pragma comment(lib, "gdi32.lib")    // brushes, pens, BitBlt, metafile, text
#pragma comment(lib, "Msimg32.lib")  // TransparentBlt

// --- Optional RPC time client -------------------------------------------
// Lights up only when the MIDL-generated stub is present, so the project
// still builds before you add the IDL. Add Timesvc.idl AND its generated
// Timesvc_c.c to this project to enable it; otherwise the clock shows FAIL.
#if defined(__has_include)
#  if __has_include("Timesvc.h")
#    include "Timesvc.h"
#    pragma comment(lib, "rpcrt4.lib")
#    define HAVE_TIME_RPC 1
#  endif
#endif

#ifdef HAVE_TIME_RPC
// RPC user allocators (needed by the stubs for any heap marshaling).
extern "C" void* __RPC_USER midl_user_allocate(size_t size) { return malloc(size); }
extern "C" void  __RPC_USER midl_user_free(void* p)         { free(p); }
#endif

#define IDM_ABOUT 1001
#define IDM_EXIT  1002
#define IDT_TITLE 1     // one-shot timer id: restore the window title
#define IDT_CLOCK 2     // periodic timer id: refresh the RPC clock (1 Hz)

static const wchar_t* kClassName = L"MinWinClass";
static const wchar_t* kTitle     = L"WASD sprite over the mountains";

// ---- Sprite state ---------------------------------------------------
static double    g_x = 120.0, g_y = 120.0;  // sprite top-left (float for smooth diag)
static const int SPRITE = 40;               // sprite diameter
static const double SPEED = 5.0;            // pixels/frame while moving

static wchar_t g_time[64] = L"FAIL";        // filled from the RPC server at startup

// ---- Mountain background (cached) -----------------------------------
#define MAXW 4096
static int   g_mtn[MAXW];        // silhouette y per column
static POINT g_pts[MAXW + 2];    // polygon points (silhouette + 2 bottom corners)
static int   g_mtnW = 0, g_mtnH = 0;   // size the cache was built for

// ---- Cached GDI objects (created in WM_CREATE, freed in WM_DESTROY) --
static HBRUSH g_skyBrush = nullptr, g_groundBrush = nullptr, g_fillBrush = nullptr;
static HPEN   g_ridgePen = nullptr, g_edgePen = nullptr;

// ---- Persistent back buffer (rebuilt only in WM_SIZE) ---------------
static HDC     g_memDC   = nullptr;   // off-screen DC
static HBITMAP g_backBmp = nullptr;   // its bitmap
static HBITMAP g_oldBmp  = nullptr;   // DC's original bitmap (restore before delete)
static int     g_bbW = 0, g_bbH = 0;  // back-buffer size
static bool    g_inSizeMove = false;  // (Improvement 4) track resize drags

// ---- Sprite bitmap --------------------------------------------------
static HDC     g_spriteDC = nullptr;
static HBITMAP g_spriteBmp = nullptr;
static HBITMAP g_oldSpriteBmp = nullptr;

LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

// ---------------------------------------------------------------------------
// 1D midpoint displacement: set the midpoint to the average of the endpoints
// plus a random offset, then recurse with a smaller offset each level.
static void Midpoint(int* a, int lo, int hi, double disp)
{
    if (hi - lo < 2) return;
    int mid = (lo + hi) / 2;
    double r = ((double)rand() / RAND_MAX * 2.0 - 1.0) * disp;
    a[mid] = (a[lo] + a[hi]) / 2 + (int)r;
    Midpoint(a, lo, mid, disp * 0.55);
    Midpoint(a, mid, hi, disp * 0.55);
}

static void GenMountain(int w, int h)
{
    if (w < 2) return;
    int base = (int)(h * 0.55);
    int amp  = (int)(h * 0.30);
    if (amp < 1) amp = 1;

    g_mtn[0]     = base + (rand() % amp - amp / 2);
    g_mtn[w - 1] = base + (rand() % amp - amp / 2);
    Midpoint(g_mtn, 0, w - 1, amp);

    int top = (int)(h * 0.15), bot = (int)(h * 0.90);   // keep ridge on-screen
    for (int x = 0; x < w; ++x) {
        if (g_mtn[x] < top) g_mtn[x] = top;
        if (g_mtn[x] > bot) g_mtn[x] = bot;
    }
}

// ---------------------------------------------------------------------------
// Recreate the off-screen buffer to match the new client size, and regenerate
// the mountain (both depend on size). Called from WM_SIZE only.
static void RebuildBackBuffer(HWND hWnd, int w, int h)
{
    if (w < 1 || h < 1) return;

    HDC wdc = GetDC(hWnd);
    if (g_memDC) {                         // release the previous buffer
        SelectObject(g_memDC, g_oldBmp);
        DeleteObject(g_backBmp);
        DeleteDC(g_memDC);
    }
    g_memDC   = CreateCompatibleDC(wdc);
    g_backBmp = CreateCompatibleBitmap(wdc, w, h);
    g_oldBmp  = (HBITMAP)SelectObject(g_memDC, g_backBmp);
    ReleaseDC(hWnd, wdc);
    g_bbW = w; g_bbH = h;

    int mw = (w < MAXW) ? w : MAXW;
    GenMountain(mw, h);
    g_mtnW = mw; g_mtnH = h;
}

// ---------------------------------------------------------------------------
// Draw the whole scene into any DC. Used both for the on-screen back buffer
// and for recording into a metafile, so a WMF capture matches the window.
static void DrawScene(HDC dc, int w, int h)
{
    int mw = (w < MAXW) ? w : MAXW;
    RECT rc = { 0, 0, w, h };

    FillRect(dc, &rc, g_skyBrush);

    for (int x = 0; x < mw; ++x) { g_pts[x].x = x; g_pts[x].y = g_mtn[x]; }
    g_pts[mw].x     = mw - 1; g_pts[mw].y     = h;
    g_pts[mw + 1].x = 0;      g_pts[mw + 1].y = h;

    HBRUSH oldB = (HBRUSH)SelectObject(dc, g_groundBrush);
    HPEN   oldP = (HPEN)SelectObject(dc, g_ridgePen);
    Polygon(dc, g_pts, mw + 2);
    SelectObject(dc, oldB);
    SelectObject(dc, oldP);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(20, 40, 60));
    static const wchar_t hint[] = L"WASD to move (diagonals too)";
    TextOutW(dc, 10, 8, hint, lstrlenW(hint));

    // Time from the RPC server, right-aligned in the top-right corner.
    // (Improvement 3: Let GDI handle right-alignment natively)
    UINT oldAlign = SetTextAlign(dc, TA_RIGHT | TA_TOP);
    TextOutW(dc, w - 10, 8, g_time, lstrlenW(g_time));
    SetTextAlign(dc, oldAlign);

    // Draw the bitmap sprite using a transparent color key (Magenta)
    TransparentBlt(dc, (int)g_x, (int)g_y, SPRITE, SPRITE, g_spriteDC, 0, 0, SPRITE, SPRITE, RGB(255, 0, 255));
}

// Record the current scene into a *placeable* WMF and write it to disk.
// A memory metafile captures the GDI calls; we then prepend the 22-byte
// Aldus placeable header (with checksum) so other apps can open the file.
static bool SaveSceneAsWmf(const wchar_t* path, int w, int h)
{
    HDC mdc = CreateMetaFileW(nullptr);        // memory-based metafile DC
    if (!mdc) return false;
    DrawScene(mdc, w, h);
    HMETAFILE hmf = CloseMetaFile(mdc);
    if (!hmf) return false;

    UINT n = GetMetaFileBitsEx(hmf, 0, nullptr);
    BYTE* bits = (BYTE*)malloc(n);
    if (!bits) { DeleteMetaFile(hmf); return false; }
    GetMetaFileBitsEx(hmf, n, bits);
    DeleteMetaFile(hmf);

#pragma pack(push, 1)
    struct APMHeader {
        DWORD key;            // 0x9AC6CDD7
        WORD  handle;         // 0
        SHORT left, top, right, bottom;
        WORD  inch;           // metafile units per inch
        DWORD reserved;       // 0
        WORD  checksum;       // XOR of the first 10 WORDs above
    } hdr;
#pragma pack(pop)
    hdr.key = 0x9AC6CDD7; hdr.handle = 0;
    hdr.left = 0; hdr.top = 0; hdr.right = (SHORT)w; hdr.bottom = (SHORT)h;
    hdr.inch = 96; hdr.reserved = 0; hdr.checksum = 0;
    WORD* wp = (WORD*)&hdr;
    WORD sum = 0;
    for (int i = 0; i < 10; ++i) sum ^= wp[i];
    hdr.checksum = sum;

    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { free(bits); return false; }
    DWORD written;
    WriteFile(f, &hdr, sizeof(hdr), &written, nullptr);
    WriteFile(f, bits, n, &written, nullptr);
    CloseHandle(f);
    free(bits);
    return true;
}

// Copy the current canvas to the clipboard as a DIB (Device-Independent Bitmap).
// (Improvement 2: CF_DIB is much more reliable across apps than CF_BITMAP).
static bool CopyCanvasToClipboard(HWND hWnd)
{
    if (!g_memDC || g_bbW < 1 || g_bbH < 1) return false;

    // Render the current frame, then copy it into a standalone bitmap.
    DrawScene(g_memDC, g_bbW, g_bbH);

    HDC     wdc  = GetDC(hWnd);
    HDC     cdc  = CreateCompatibleDC(wdc);
    HBITMAP copy = CreateCompatibleBitmap(wdc, g_bbW, g_bbH);
    HBITMAP oldc = (HBITMAP)SelectObject(cdc, copy);
    BitBlt(cdc, 0, 0, g_bbW, g_bbH, g_memDC, 0, 0, SRCCOPY);
    SelectObject(cdc, oldc);          // deselect before extracting bits

    // Prepare BITMAPINFO for a 32-bpp bottom-up DIB
    BITMAPINFO bmi = { 0 };
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = g_bbW;
    bmi.bmiHeader.biHeight = g_bbH;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    // First call to fill in biSizeImage
    GetDIBits(wdc, copy, 0, g_bbH, nullptr, &bmi, DIB_RGB_COLORS);
    if (bmi.bmiHeader.biSizeImage == 0)
        bmi.bmiHeader.biSizeImage = g_bbW * 4 * g_bbH;

    // Allocate movable global memory for the clipboard
    HANDLE hMem = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + bmi.bmiHeader.biSizeImage);
    if (hMem) {
        BITMAPINFO* pBmi = (BITMAPINFO*)GlobalLock(hMem);
        *pBmi = bmi;
        // Second call to actually get the pixels
        GetDIBits(wdc, copy, 0, g_bbH, (BYTE*)pBmi + sizeof(BITMAPINFOHEADER), pBmi, DIB_RGB_COLORS);
        GlobalUnlock(hMem);
    }

    DeleteDC(cdc);
    ReleaseDC(hWnd, wdc);
    DeleteObject(copy); // We are transferring hMem to the clipboard, not the HBITMAP

    bool ok = false;
    if (hMem && OpenClipboard(hWnd)) {
        EmptyClipboard();
        // On success the clipboard OWNS 'hMem' -- do not free it.
        if (SetClipboardData(CF_DIB, hMem)) ok = true;
        else GlobalFree(hMem);      // hand-off failed: we still own it
        CloseClipboard();
    } else if (hMem) {
        GlobalFree(hMem);           // couldn't open clipboard: clean up
    }
    return ok;
}

// ---------------------------------------------------------------------------
// RPC time client. The binding is created ONCE and reused for every 1 Hz
// refresh -- ncalrpc binding is lazy (no connection until the first call), so
// binding succeeds even if the server starts later; only the call fails while
// the server is down, and it recovers automatically once it's up.
#ifdef HAVE_TIME_RPC
static bool g_bound = false;

static bool EnsureBinding(void)
{
    if (g_bound) return true;

    RPC_WSTR strBinding = nullptr;
    if (RpcStringBindingComposeW(nullptr, (RPC_WSTR)L"ncalrpc", nullptr,
            (RPC_WSTR)L"WinApiDemoTime", nullptr, &strBinding) != RPC_S_OK)
        return false;

    RPC_STATUS s = RpcBindingFromStringBindingW(strBinding, &Timesvc_Binding);
    RpcStringFreeW(&strBinding);
    g_bound = (s == RPC_S_OK);
    return g_bound;
}

static void FreeBinding(void)
{
    if (g_bound && Timesvc_Binding) {
        RpcBindingFree(&Timesvc_Binding);   // also nulls the handle
        g_bound = false;
    }
}
#endif

// Refresh g_time from the server. On any failure it becomes "FAIL".
static void UpdateTimeViaRpc(void)
{
#ifdef HAVE_TIME_RPC
    if (!EnsureBinding()) { lstrcpynW(g_time, L"FAIL", 64); return; }

    // The call must be guarded: transport/server errors are raised as
    // structured exceptions, not return codes.
    RpcTryExcept
    {
        wchar_t buf[64] = { 0 };
        if (GetLocalTimeString(buf) == 0 && buf[0] != L'\0')
            lstrcpynW(g_time, buf, 64);
        else
            lstrcpynW(g_time, L"FAIL", 64);
    }
    RpcExcept(1)   // 1 = handle every RPC exception
    {
        lstrcpynW(g_time, L"FAIL", 64);
    }
    RpcEndExcept
#else
    lstrcpynW(g_time, L"FAIL", 64);   // stub not built into this project
#endif
}

// ---------------------------------------------------------------------------
int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
                      _In_opt_ HINSTANCE hPrevInstance,
                      _In_ LPWSTR lpCmdLine,
                      _In_ int nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

    srand(GetTickCount());   // new mountain each run

    WNDCLASSEXW wcex = { 0 };
    wcex.cbSize        = sizeof(wcex);
    wcex.style         = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc   = WndProc;
    wcex.hInstance     = hInstance;
    wcex.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground = nullptr;         // we paint every pixel ourselves
    wcex.lpszClassName = kClassName;
    if (!RegisterClassExW(&wcex))
        return FALSE;

    HMENU hMenu = CreateMenu();
    HMENU hFile = CreatePopupMenu();
    HMENU hHelp = CreatePopupMenu();
    AppendMenuW(hFile, MF_STRING, IDM_EXIT,  L"E&xit");
    AppendMenuW(hHelp, MF_STRING, IDM_ABOUT, L"&About");
    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hFile, L"&File");
    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hHelp, L"&Help");

    HWND hWnd = CreateWindowExW(
        0, kClassName, kTitle,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, 0, 640, 480,
        nullptr, hMenu, hInstance, nullptr);
    if (!hWnd)
        return FALSE;

    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);

    UpdateTimeViaRpc();                          // first reading immediately
    SetTimer(hWnd, IDT_CLOCK, 1000, nullptr);    // then refresh once a second

    // (Improvement 1: Setup QPC for a proper game loop timer)
    LARGE_INTEGER freq, lastTime;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&lastTime);

    // ---- Game loop -----------------------------------------------------
    MSG msg = { 0 };
    for (;;) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT)
                goto done;
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        // (Improvement 1: Use delta time to measure frame intervals without blocking the pump)
        LARGE_INTEGER currentTime;
        QueryPerformanceCounter(&currentTime);
        double dt = (double)(currentTime.QuadPart - lastTime.QuadPart) / freq.QuadPart;

        const double frameTime = 1.0 / 60.0;
        if (dt < frameTime) {
            DWORD waitTime = (DWORD)((frameTime - dt) * 1000.0);
            if (waitTime > 0) {
                // Sleep remaining time, but wake immediately if a window message arrives
                MsgWaitForMultipleObjects(0, nullptr, FALSE, waitTime, QS_ALLINPUT);
            }
            continue; 
        }

        // If the window was dragged or paused, clamp delta time so we don't jump too far
        if (dt > 0.1) dt = frameTime;
        lastTime = currentTime;

        // Apply a time scale since SPEED was originally tuned for exactly 16.6ms frames
        double timeScale = dt / frameTime;

        // Build a direction vector from held keys, then normalize it so any
        // direction (cardinal or diagonal) moves exactly SPEED px/frame.
        double dx = 0.0, dy = 0.0;
        if (GetAsyncKeyState('W') & 0x8000) dy -= 1.0;
        if (GetAsyncKeyState('S') & 0x8000) dy += 1.0;
        if (GetAsyncKeyState('A') & 0x8000) dx -= 1.0;
        if (GetAsyncKeyState('D') & 0x8000) dx += 1.0;

        double len = sqrt(dx * dx + dy * dy);
        if (len > 0.0) {
            g_x += dx / len * SPEED * timeScale;
            g_y += dy / len * SPEED * timeScale;
        }

        RECT rc;
        GetClientRect(hWnd, &rc);

        // Clamp the sprite to the client area. Clamping is just
        // max(lo, min(hi, v)), which SSE2 does branchlessly with MAXSD/MINSD
        // on scalar doubles. Precompute the upper bounds as ints so the asm
        // only has to reference simple locals (not struct members / consts).
        int maxX = rc.right  - SPRITE;
        int maxY = rc.bottom - SPRITE;
#if defined(_M_IX86)   // MSVC inline __asm is 32-bit x86 only
        __asm {
            xorpd    xmm2, xmm2      ; xmm2 = 0.0  (lower bound)

            movsd    xmm0, g_x       ; xmm0 = g_x
            maxsd    xmm0, xmm2      ; xmm0 = max(g_x, 0)
            cvtsi2sd xmm1, maxX      ; xmm1 = (double)(rc.right - SPRITE)
            minsd    xmm0, xmm1      ; xmm0 = min(xmm0, maxX)
            movsd    g_x, xmm0       ; g_x  = clamped value

            movsd    xmm0, g_y       ; xmm0 = g_y
            maxsd    xmm0, xmm2      ; xmm0 = max(g_y, 0)
            cvtsi2sd xmm1, maxY      ; xmm1 = (double)(rc.bottom - SPRITE)
            minsd    xmm0, xmm1      ; xmm0 = min(xmm0, maxY)
            movsd    g_y, xmm0       ; g_y  = clamped value
        }
#else                  // x64: no __asm -> same MAXSD/MINSD via SSE2 intrinsics
        __m128d lo = _mm_setzero_pd();                 // 0.0
        __m128d vx = _mm_set_sd(g_x);
        vx = _mm_max_sd(vx, lo);                       // max(g_x, 0)  -> MAXSD
        vx = _mm_min_sd(vx, _mm_set_sd((double)maxX)); // min(., maxX) -> MINSD
        g_x = _mm_cvtsd_f64(vx);

        __m128d vy = _mm_set_sd(g_y);
        vy = _mm_max_sd(vy, lo);
        vy = _mm_min_sd(vy, _mm_set_sd((double)maxY));
        g_y = _mm_cvtsd_f64(vy);
#endif

        InvalidateRect(hWnd, nullptr, FALSE);
        UpdateWindow(hWnd);
    }
done:
    return (int)msg.wParam;
}

// ---------------------------------------------------------------------------
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_CREATE: {
        // Allocate GDI objects once for the window's lifetime.
        g_skyBrush    = CreateSolidBrush(RGB(0, 190, 230));
        g_groundBrush = CreateSolidBrush(RGB(235, 205, 70));
        g_fillBrush   = CreateSolidBrush(RGB(220, 40, 80));
        g_ridgePen    = CreatePen(PS_SOLID, 2, RGB(110, 80, 30));
        g_edgePen     = CreatePen(PS_SOLID, 2, RGB(255, 240, 240));

        // Create the bitmap sprite (diamond shape)
        HDC wdc = GetDC(hWnd);
        g_spriteDC = CreateCompatibleDC(wdc);
        g_spriteBmp = CreateCompatibleBitmap(wdc, SPRITE, SPRITE);
        g_oldSpriteBmp = (HBITMAP)SelectObject(g_spriteDC, g_spriteBmp);
        ReleaseDC(hWnd, wdc);

        // Fill background with magenta (used as transparent color key)
        RECT rc = { 0, 0, SPRITE, SPRITE };
        HBRUSH magBrush = CreateSolidBrush(RGB(255, 0, 255));
        FillRect(g_spriteDC, &rc, magBrush);
        DeleteObject(magBrush);

        // Draw the diamond
        HBRUSH oldB = (HBRUSH)SelectObject(g_spriteDC, g_fillBrush);
        HPEN oldP = (HPEN)SelectObject(g_spriteDC, g_edgePen);
        POINT pts[4] = { {SPRITE/2, 1}, {SPRITE-2, SPRITE/2}, {SPRITE/2, SPRITE-2}, {1, SPRITE/2} };
        Polygon(g_spriteDC, pts, 4);
        SelectObject(g_spriteDC, oldB);
        SelectObject(g_spriteDC, oldP);
        return 0;
    }

    case WM_ENTERSIZEMOVE:
        // (Improvement 4: Pause heavy resizing operations during drag)
        g_inSizeMove = true;
        return 0;

    case WM_EXITSIZEMOVE:
        // (Improvement 4: Rebuild the buffer to the final size once drag finishes)
        g_inSizeMove = false;
        {
            RECT rc;
            GetClientRect(hWnd, &rc);
            RebuildBackBuffer(hWnd, rc.right, rc.bottom);
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        return 0;

    case WM_SIZE:
        // (Improvement 4: Only rebuild immediately if we aren't dragging the window edges)
        if (!g_inSizeMove) {
            RebuildBackBuffer(hWnd, LOWORD(lParam), HIWORD(lParam));
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDM_ABOUT:
            MessageBoxW(hWnd,
                L"Move with W / A / S / D (diagonals supported).",
                L"About", MB_OK | MB_ICONINFORMATION);
            break;
        case IDM_EXIT:
            DestroyWindow(hWnd);
            break;
        default:
            return DefWindowProc(hWnd, message, wParam, lParam);
        }
        break;

    case WM_ERASEBKGND:
        return 1;   // WM_PAINT covers every pixel; skip the flicker-y erase

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            // Capture the current frame to a placeable WMF, then quit.
            if (!SaveSceneAsWmf(L"capture.wmf", g_bbW, g_bbH))
                MessageBoxW(hWnd, L"Failed to write capture.wmf",
                            L"Capture", MB_OK | MB_ICONERROR);
            DestroyWindow(hWnd);
        }
        else if (wParam == VK_TAB) {
            // Copy the canvas to the clipboard; paste as an image elsewhere.
            // Flash a confirmation in the title bar for ~1 second.
            if (CopyCanvasToClipboard(hWnd)) {
                SetWindowTextW(hWnd, L"Copied to clipboard!");
                SetTimer(hWnd, IDT_TITLE, 1000, nullptr);   // one-shot restore
            }
        }
        return 0;

    case WM_TIMER:
        if (wParam == IDT_TITLE) {
            KillTimer(hWnd, IDT_TITLE);       // fire once, then stop
            SetWindowTextW(hWnd, kTitle);     // restore the real title
        }
        else if (wParam == IDT_CLOCK) {
            UpdateTimeViaRpc();               // re-query; game loop repaints it
        }
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        if (g_memDC) {                     // guard: buffer exists after WM_SIZE
            DrawScene(g_memDC, g_bbW, g_bbH);          // render off-screen
            BitBlt(hdc, 0, 0, g_bbW, g_bbH, g_memDC, 0, 0, SRCCOPY);  // one blit
        }
        EndPaint(hWnd, &ps);
        break;
    }

    case WM_DESTROY:
        KillTimer(hWnd, IDT_CLOCK);
#ifdef HAVE_TIME_RPC
        FreeBinding();                     // release the RPC binding handle
#endif
        if (g_memDC) {                     // tear down the back buffer
            SelectObject(g_memDC, g_oldBmp);
            DeleteObject(g_backBmp);
            DeleteDC(g_memDC);
            g_memDC = nullptr;
        }
        if (g_spriteDC) {                  // tear down the sprite
            SelectObject(g_spriteDC, g_oldSpriteBmp);
            DeleteObject(g_spriteBmp);
            DeleteDC(g_spriteDC);
            g_spriteDC = nullptr;
        }
        DeleteObject(g_skyBrush);
        DeleteObject(g_groundBrush);
        DeleteObject(g_fillBrush);
        DeleteObject(g_ridgePen);
        DeleteObject(g_edgePen);
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}
