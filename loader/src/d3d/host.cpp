#include "d3d/host.h"

#include "settings.h"
#include "perf.h"
#include "d3d/draw.h"

#include <shellapi.h>

#include <algorithm>
#include <string>

// Common controls 6 (themed message boxes): a manifest dependency the linker
// embeds in blackrt.dll as resource 2; ui_thread activates it for every window it makes.
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' " \
                        "processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace host3d {
namespace {

HWND g_hwnd;
HANDLE g_window_ready;
std::atomic<bool> g_closed{false};
IDirect3D9 *g_d3d;
bool g_intz;
IDirect3DDevice9 *g_dev;
bool g_in_scene;
std::atomic<int> g_win_w, g_win_h; // client area, follows WM_SIZE
int g_aspect_w = 4, g_aspect_h = 3; // the picture's shape; the window keeps it when resized
int g_bb_w, g_bb_h;                 // host back buffer: the largest monitor, so any window size is drawn 1:1
int g_frame_w, g_frame_h;           // the part of the back buffer the current frame uses
WINDOWPLACEMENT g_windowed_place = {sizeof(WINDOWPLACEMENT)};

// Mouse: raw input counts and buttons, used only while the cursor is captured (click the window; focus loss
// releases it). The click that captures is not passed to the game.
std::atomic<bool> g_captured{false};
// While a menu page is up the menus own the cursor (input/bindings.cpp sets this each poll): a click there must not
// capture the mouse, which would hide the cursor for a frame and swallow the click itself (see WM_LBUTTONDOWN).
std::atomic<bool> g_cursor_free{false};
std::atomic<int> g_mouse_dx, g_mouse_dy, g_wheel, g_buttons;
// A menu click is latched when the window hears it, never polled: the input poll runs once per game frame, so a quick
// press and release can fall entirely between two polls. Bit 0 is the left button and bit 1 the right; the held state
// is what keeps a hold repeating.
std::atomic<int> g_menu_press{0}, g_menu_hold{0};
// Wheel notches while a menu page is up: the menus walk the highlight with them as the up/down arrows do
// (input/bindings.cpp), so they are latched here and not lost between the front end's own updates.
std::atomic<int> g_menu_wheel{0};
bool g_hold_left; // the capturing click: masked until released
constexpr UINT WM_APP_CAPTURE = WM_APP + 0x52; // wParam: 1 = capture, 0 = release (request_capture)
constexpr UINT WM_APP_FULLSCREEN = WM_APP + 0x53; // wParam: 1 = fullscreen, 0 = windowed (set_fullscreen)
constexpr UINT WM_APP_WINSIZE = WM_APP + 0x54; // wParam/lParam: client width/height, 0 = fit (set_window_size)
int g_pending_w, g_pending_h; // RESOLUTION picked while fullscreen: applied when the window is back
bool g_pending_size;

bool is_fullscreen();

// The client size of the picture's aspect that fills the desktop's work area (the task bar and the window's own
// frame aside), in whole multiples of the aspect so the picture stays exact.
void fit_client_size(int *w, int *h)
{
    RECT f = {0, 0, 0, 0};
    AdjustWindowRect(&f, WS_OVERLAPPEDWINDOW, FALSE);
    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int avail_w = std::max(1, (int)(work.right - work.left - (f.right - f.left)));
    int avail_h = std::max(1, (int)(work.bottom - work.top - (f.bottom - f.top)));
    int step = g_aspect_h * 2;
    int height = avail_h / step * step;
    int width = height * g_aspect_w / g_aspect_h;
    if (width > avail_w) {
        step = g_aspect_w * 2;
        width = avail_w / step * step;
        height = width * g_aspect_h / g_aspect_w;
    }
    *w = std::max(width, 320);
    *h = std::max(height, 240);
}

// PC SETTINGS > VIDEO > RESOLUTION: a new client size, keeping the window's place (clamped so it stays on the
// monitor's work area). Zero means the desktop fit. Fullscreen is the whole monitor by design, so the size is
// only remembered there and applied when the window comes back.
void apply_window_size(int cw, int ch)
{
    if (!g_hwnd || is_fullscreen()) return;
    if (cw <= 0 && ch <= 0) fit_client_size(&cw, &ch);
    else if (ch <= 0) ch = cw * g_aspect_h / g_aspect_w;
    else if (cw <= 0) cw = ch * g_aspect_w / g_aspect_h;
    RECT wr;
    GetWindowRect(g_hwnd, &wr);
    RECT r = {0, 0, cw, ch};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    MONITORINFO mi = {sizeof mi};
    if (GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY), &mi)) work = mi.rcWork;
    int w = r.right - r.left, h = r.bottom - r.top;
    int x = std::min(std::max(wr.left, work.left), std::max(work.left, work.right - w));
    int y = std::min(std::max(wr.top, work.top), std::max(work.top, work.bottom - h));
    SetWindowPos(g_hwnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOOWNERZORDER);
    logf("host: window client %dx%d", cw, ch);
}

// Borderless window over the whole monitor, or back to the window it was.
void toggle_fullscreen()
{
    DWORD style = GetWindowLongW(g_hwnd, GWL_STYLE);
    if (style & WS_OVERLAPPEDWINDOW) {
        MONITORINFO mi = {sizeof mi};
        if (!GetWindowPlacement(g_hwnd, &g_windowed_place) ||
            !GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY), &mi))
            return;
        SetWindowLongW(g_hwnd, GWL_STYLE, (style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        g_cfg.fullscreen = true;
        SetWindowPos(g_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        logf("host: fullscreen %ldx%ld", mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top);
    } else {
        SetWindowLongW(g_hwnd, GWL_STYLE, (style & ~WS_POPUP) | WS_OVERLAPPEDWINDOW);
        g_cfg.fullscreen = false;
        SetWindowPlacement(g_hwnd, &g_windowed_place);
        SetWindowPos(g_hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        if (g_pending_size) { // RESOLUTION was chosen while fullscreen
            g_pending_size = false;
            apply_window_size(g_pending_w, g_pending_h);
        }
        logf("host: windowed");
    }
}

// Window border sizes (window rect minus client rect).
void frame_extra(int *fw, int *fh)
{
    RECT wr, cr;
    GetWindowRect(g_hwnd, &wr);
    GetClientRect(g_hwnd, &cr);
    *fw = (wr.right - wr.left) - cr.right;
    *fh = (wr.bottom - wr.top) - cr.bottom;
}

// WM_SIZING: keep the client area at the picture's aspect. Edges drive the other dimension; corners follow
// whichever dimension was dragged further.
void keep_aspect(WPARAM edge, RECT *r)
{
    int fw, fh;
    frame_extra(&fw, &fh);
    int cw = r->right - r->left - fw, ch = r->bottom - r->top - fh;
    if (edge == WMSZ_LEFT || edge == WMSZ_RIGHT) ch = cw * g_aspect_h / g_aspect_w;
    else if (edge == WMSZ_TOP || edge == WMSZ_BOTTOM) cw = ch * g_aspect_w / g_aspect_h;
    else {
        cw = std::max(cw, ch * g_aspect_w / g_aspect_h);
        ch = cw * g_aspect_h / g_aspect_w;
    }
    if (edge == WMSZ_LEFT || edge == WMSZ_TOPLEFT || edge == WMSZ_BOTTOMLEFT) r->left = r->right - cw - fw;
    else r->right = r->left + cw + fw;
    if (edge == WMSZ_TOP || edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT) r->top = r->bottom - ch - fh;
    else r->bottom = r->top + ch + fh;
}

void clip_to_client()
{
    RECT r;
    GetClientRect(g_hwnd, &r);
    MapWindowPoints(g_hwnd, nullptr, (POINT *)&r, 2);
    ClipCursor(&r);
}

void capture_mouse(bool on)
{
    g_menu_press = 0; // a menu click never crosses into or out of play
    g_menu_hold = 0;
    if (on == g_captured) return;
    g_captured = on;
    if (on) {
        clip_to_client();
        SetCursor(nullptr);
        g_hold_left = true;
    } else {
        ClipCursor(nullptr);
        g_buttons = 0;
    }
    g_mouse_dx = g_mouse_dy = g_wheel = 0;
}

void raw_input(HRAWINPUT h)
{
    alignas(8) BYTE buf[sizeof(RAWINPUT) + 16];
    UINT size = sizeof buf;
    if (GetRawInputData(h, RID_INPUT, buf, &size, sizeof(RAWINPUTHEADER)) == (UINT)-1) return;
    const RAWINPUT *ri = (const RAWINPUT *)buf;
    if (ri->header.dwType != RIM_TYPEMOUSE || !g_captured) return;
    const RAWMOUSE &m = ri->data.mouse;
    if (!(m.usFlags & MOUSE_MOVE_ABSOLUTE)) {
        g_mouse_dx += m.lLastX;
        g_mouse_dy += m.lLastY;
    }
    USHORT f = m.usButtonFlags;
    int b = g_buttons;
    for (int i = 0; i < 5; i++) { // left, right, middle, X1, X2: down/up flag pairs
        if (f & (1 << (2 * i))) b |= 1 << i;
        if (f & (2 << (2 * i))) b &= ~(1 << i);
    }
    if (f & RI_MOUSE_LEFT_BUTTON_UP) g_hold_left = false;
    if (g_hold_left) b &= ~1;
    g_buttons = b;
    if (f & RI_MOUSE_WHEEL) { // notches (smooth-scrolling wheels send fractions: count each burst as one)
        SHORT d = (SHORT)m.usButtonData;
        g_wheel += d / WHEEL_DELTA ? d / WHEEL_DELTA : (d > 0 ? 1 : -1);
    }
}

bool is_fullscreen() { return !(GetWindowLongW(g_hwnd, GWL_STYLE) & WS_OVERLAPPEDWINDOW); }

// A performance recording was saved (perf.cpp): windowed, offer to open its folder; fullscreen, the title says it.
void perf_report(HWND hwnd)
{
    if (is_fullscreen() || g_cfg.background) return;
    std::wstring text = widen(perf::last_report_text());
    text += L"\n\nSaved in ";
    text += perf::last_report_dir();
    text += L"\n\nOpen the folder?";
    if (MessageBoxW(hwnd, text.c_str(), L"Performance recording", MB_YESNO | MB_ICONINFORMATION) == IDYES)
        ShellExecuteW(hwnd, L"open", perf::last_report_dir(), nullptr, nullptr, SW_SHOWNORMAL);
}

LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CLOSE:
        g_closed = true;
        settings_save();
        request_exit(0, "window closed");
        return 0;
    case WM_APP_FULLSCREEN: // PC SETTINGS > VIDEO > FULLSCREEN (fe/pc_settings.cpp)
        if ((wp != 0) != is_fullscreen()) {
            toggle_fullscreen();
            settings_save();
        }
        return 0;
    case WM_APP_WINSIZE: // PC SETTINGS > VIDEO > RESOLUTION (fe/pc_settings.cpp)
        if (is_fullscreen()) {
            g_pending_w = (int)wp;
            g_pending_h = (int)lp;
            g_pending_size = true;
        } else {
            apply_window_size((int)wp, (int)lp);
        }
        settings_save();
        return 0;
    case perf::WM_APP_PERF:
        capture_mouse(false);
        perf_report(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED && LOWORD(lp) && HIWORD(lp)) {
            g_win_w = LOWORD(lp);
            g_win_h = HIWORD(lp);
        }
        if (g_captured) clip_to_client();
        return 0;
    case WM_MOVE:
        if (g_captured) clip_to_client();
        break;
    case WM_SIZING:
        keep_aspect(wp, (RECT *)lp);
        return TRUE;
    case WM_GETMINMAXINFO:
        if (g_hwnd) {
            int fw, fh;
            frame_extra(&fw, &fh);
            ((MINMAXINFO *)lp)->ptMinTrackSize = {320 + fw, 320 * g_aspect_h / g_aspect_w + fh};
        }
        return 0;
    case WM_APP_CAPTURE: // from the input poll: pause frees the mouse, resuming play takes it again
        if (!wp) capture_mouse(false);
        else if (GetForegroundWindow() == h && !IsIconic(h)) capture_mouse(true);
        return 0;
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_KEYMENU) return 0; // Alt / F10 must not open the system menu by keyboard and stall input
        break;
    case WM_LBUTTONDOWN:
        if (!g_cursor_free.load()) capture_mouse(true); // a menu page owns the cursor: its clicks are the menu's
        else g_menu_press |= 1, g_menu_hold |= 1;
        return 0;
    case WM_LBUTTONUP:
        g_menu_hold &= ~1;
        return 0;
    case WM_RBUTTONDOWN:
        if (g_cursor_free.load()) g_menu_press |= 2, g_menu_hold |= 2; // in play the right button is the aim, not a menu's
        return 0;
    case WM_RBUTTONUP:
        g_menu_hold &= ~2;
        return 0;
    case WM_MOUSEWHEEL:
        // A menu page owns the pointer: its notches walk the highlight. In play the wheel is a bound key
        // (input/bindings.cpp), read through raw input, and this copy is left alone.
        if (g_cursor_free.load() && GetForegroundWindow() == h) {
            int d = GET_WHEEL_DELTA_WPARAM(wp);
            g_menu_wheel += d > 0 ? 1 : d < 0 ? -1 : 0;
            return 0;
        }
        break;
    case WM_CAPTURECHANGED: // another window took the mouse: no button of ours is still held
        g_menu_press = 0;
        g_menu_hold = 0;
        break;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) capture_mouse(false);
        break;
    case WM_KILLFOCUS:
        capture_mouse(false);
        break;
    case WM_SETCURSOR:
        // The Windows cursor never shows over the window: in play it is captured (mouse look), and everywhere else
        // the port draws its own arrow (d3d/cursor.cpp). Only a window that is not the one in front keeps it.
        if (LOWORD(lp) == HTCLIENT && GetForegroundWindow() == g_hwnd) {
            SetCursor(nullptr);
            return TRUE;
        }
        break;
    case WM_INPUT:
        raw_input((HRAWINPUT)lp);
        break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

DWORD WINAPI ui_thread(void *)
{
    using SetDesc = HRESULT(WINAPI *)(HANDLE, PCWSTR);
    if (auto set_desc = (SetDesc)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription"))
        set_desc(GetCurrentThread(), L"window (messages, raw mouse)");
    ACTCTXW ac = {sizeof ac};
    ac.dwFlags = ACTCTX_FLAG_HMODULE_VALID | ACTCTX_FLAG_RESOURCE_NAME_VALID;
    ac.hModule = GetModuleHandleW(L"blackrt.dll");
    ac.lpResourceName = MAKEINTRESOURCEW(2);
    HANDLE ctx = CreateActCtxW(&ac);
    ULONG_PTR cookie = 0;
    if (ctx == INVALID_HANDLE_VALUE || !ActivateActCtx(ctx, &cookie))
        logf("host: common controls 6 not activated (%lu); dialogs keep the classic look", GetLastError());
    WNDCLASSW wc = {};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(L"blackrt.dll");
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"BlackLoaderWindow";
    RegisterClassW(&wc);
    RECT r = {0, 0, g_win_w, g_win_h};
    DWORD style = WS_OVERLAPPEDWINDOW;
    AdjustWindowRect(&r, style, FALSE);
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"BLACK", style, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left,
                             r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
    for (int big = 0; big < 2; big++) { // the port's icon (res/black.rc) on the title bar and the taskbar
        int s = GetSystemMetrics(big ? SM_CXICON : SM_CXSMICON);
        if (HANDLE icon = LoadImageW(wc.hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON, s, s, 0))
            SendMessageW(g_hwnd, WM_SETICON, big ? ICON_BIG : ICON_SMALL, (LPARAM)icon);
    }
    RAWINPUTDEVICE mouse = {0x01, 0x02, 0, g_hwnd}; // generic desktop / mouse, while the window has focus
    if (!RegisterRawInputDevices(&mouse, 1, sizeof mouse)) logf("host: raw mouse input not available (%lu)", GetLastError());
    ShowWindow(g_hwnd, g_cfg.background ? SW_SHOWMINNOACTIVE : SW_SHOWNORMAL);
    if (g_cfg.fullscreen) toggle_fullscreen();
    SetEvent(g_window_ready);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}

BOOL CALLBACK grow_to_monitor(HMONITOR, HDC, LPRECT r, LPARAM p)
{
    auto *s = (SIZE *)p;
    s->cx = std::max(s->cx, r->right - r->left);
    s->cy = std::max(s->cy, r->bottom - r->top);
    return TRUE;
}

} // namespace

// Real screen pixels (no DPI bitmap stretching by Windows on scaled displays).
void make_dpi_aware()
{
    typedef BOOL(WINAPI * PFN_SetContext)(HANDLE);
    HMODULE u = GetModuleHandleW(L"user32.dll");
    auto set_ctx = u ? (PFN_SetContext)GetProcAddress(u, "SetProcessDpiAwarenessContext") : nullptr;
    if (!set_ctx || !set_ctx((HANDLE)-4 /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */)) SetProcessDPIAware();
}

bool open_window(int width, int height, int aspect_w, int aspect_h)
{
    if (g_hwnd) return true;
    make_dpi_aware();
    g_aspect_w = aspect_w;
    g_aspect_h = aspect_h;
    if (width > 0 && height <= 0) height = width * aspect_h / aspect_w;
    if (height > 0 && width <= 0) width = height * aspect_w / aspect_h; // RESOLUTION: a height alone
    if (width <= 0 || height <= 0) { // largest client area of the aspect that fits the work area comfortably
        RECT work;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        int avail_h = (work.bottom - work.top) * 9 / 10, avail_w = (work.right - work.left) * 9 / 10;
        int step = aspect_h * 2; // whole multiples of the aspect
        height = avail_h / step * step;
        width = height * aspect_w / aspect_h;
        if (width > avail_w) {
            step = aspect_w * 2;
            width = avail_w / step * step;
            height = width * aspect_h / aspect_w;
        }
    }
    g_win_w = width;
    g_win_h = height;
    g_window_ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    CreateThread(nullptr, 0, ui_thread, nullptr, 0, nullptr);
    WaitForSingleObject(g_window_ready, 10000);
    if (!g_hwnd) {
        logf("host: could not create the window");
        return false;
    }
    logf("host: window %dx%d (resizable at %d:%d)", width, height, aspect_w, aspect_h);
    return true;
}

HWND window() { return g_hwnd; }

void set_title(const char *text)
{
    if (g_hwnd) SetWindowTextA(g_hwnd, text);
}

bool window_closed() { return g_closed; }

void frame_layout(RECT *frame, RECT *picture)
{
    int cw = std::max(1, g_win_w.load()), ch = std::max(1, g_win_h.load());
    if (cw > g_bb_w || ch > g_bb_h) { // larger than every monitor was at start: draw smaller, Present stretches
        float k = std::min((float)g_bb_w / cw, (float)g_bb_h / ch);
        cw = std::max(1, (int)(cw * k));
        ch = std::max(1, (int)(ch * k));
    }
    g_frame_w = cw;
    g_frame_h = ch;
    *frame = {0, 0, cw, ch};
    int pw = cw, ph = cw * g_aspect_h / g_aspect_w;
    if (ph > ch) {
        ph = ch;
        pw = ch * g_aspect_w / g_aspect_h;
    }
    int x = (cw - pw) / 2, y = (ch - ph) / 2;
    *picture = {x, y, x + pw, y + ph};
}

bool mouse_captured() { return g_captured; }

void request_capture(bool on)
{
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_CAPTURE, on ? 1 : 0, 0);
}

void set_fullscreen(bool on)
{
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_FULLSCREEN, on ? 1 : 0, 0);
}

bool fullscreen() { return g_hwnd && is_fullscreen(); }

void set_window_size(int width, int height)
{
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_WINSIZE, (WPARAM)width, (LPARAM)height);
}

int mouse_buttons() { return g_captured ? g_buttons.load() : 0; }

void set_cursor_free(bool on)
{
    if (!on) g_menu_wheel = 0; // a notch scrolled before the menus went away is not for the next visit
    g_cursor_free = on;
}

// Wheel notches while a menu page owns the pointer (WM_MOUSEWHEEL): + = away from the user (up), - = towards it.
int menu_wheel() { return g_menu_wheel.exchange(0); }

namespace {
std::atomic<float> g_test_x{-1.0f}, g_test_y{-1.0f}; // tests: --press-file "ptr:X,Y" (stage units), "ptr:off"

// The pointer in the front end's 640x480 stage (the picture keeps the game's aspect, centred in the client area).
// clamp: a point outside the picture goes to its edge instead of failing.
bool pointer_in_stage(float *x, float *y, bool clamp)
{
    if (g_test_x.load() >= 0.0f) {
        *x = g_test_x.load();
        *y = g_test_y.load();
        return true;
    }
    if (!g_hwnd || g_captured) return false;
    if (GetForegroundWindow() != g_hwnd || IsIconic(g_hwnd)) return false; // only the window in front drives its menus
    POINT p;
    if (!GetCursorPos(&p) || !ScreenToClient(g_hwnd, &p)) return false;
    int cw = g_win_w.load(), ch = g_win_h.load();
    if (cw <= 0 || ch <= 0) return false;
    int pw = cw, ph = pw * g_aspect_h / g_aspect_w;
    if (ph > ch) {
        ph = ch;
        pw = ph * g_aspect_w / g_aspect_h;
    }
    int px = (cw - pw) / 2, py = (ch - ph) / 2;
    if (!clamp && (p.x < px || p.y < py || p.x >= px + pw || p.y >= py + ph)) return false;
    float sx = (float)(p.x - px) * 640.0f / (float)pw, sy = (float)(p.y - py) * 480.0f / (float)ph;
    *x = std::clamp(sx, 0.0f, 640.0f);
    *y = std::clamp(sy, 0.0f, 480.0f);
    return true;
}
} // namespace

void test_pointer(float x, float y)
{
    g_test_y = y;
    g_test_x = x;
}

bool cursor_stage(float *x, float *y) { return pointer_in_stage(x, y, false); }
bool cursor_stage_any(float *x, float *y) { return pointer_in_stage(x, y, true); }
bool cursor_free() { return g_cursor_free.load(); }

bool cursor_down(int button)
{
    if (!g_hwnd || g_captured || GetForegroundWindow() != g_hwnd) return false;
    int bit = 1 << (button ? 1 : 0);
    int press = g_menu_press.fetch_and(~bit) & bit; // the press since the last poll (taken: a fast click is not lost)
    return press != 0 || (g_menu_hold.load() & bit) != 0;
}

void mouse_take(int *dx, int *dy, int *wheel)
{
    *dx = g_mouse_dx.exchange(0);
    *dy = g_mouse_dy.exchange(0);
    *wheel = g_wheel.exchange(0);
}

D3DPRESENT_PARAMETERS g_pp; // as created; Reset uses the same

bool create_device()
{
    g_d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!g_d3d) {
        logf("host: Direct3DCreate9 failed");
        return false;
    }
    SIZE mon = {g_win_w, g_win_h};
    EnumDisplayMonitors(nullptr, nullptr, grow_to_monitor, (LPARAM)&mon);
    g_bb_w = mon.cx;
    g_bb_h = mon.cy;
    D3DPRESENT_PARAMETERS pp = {};
    pp.BackBufferWidth = g_bb_w;
    pp.BackBufferHeight = g_bb_h;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_COPY; // COPY: Present can show just the part of the back buffer a frame used
    pp.hDeviceWindow = g_hwnd;
    pp.Windowed = TRUE;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    // Frames are paced by the emulated Xbox vblank (D3DDevice_Swap); the host monitor's refresh must not add a second
    // clock unless V-SYNC (PC SETTINGS > MODS) asks Present to wait for it.
    pp.PresentationInterval = g_cfg.vsync ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;
    // FPU_PRESERVE: the game's x87 math must keep its precision; MULTITHREADED: loader threads may call in.
    DWORD flags = D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE | D3DCREATE_MULTITHREADED;
    g_pp = pp;
    HRESULT hr = g_d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, g_hwnd, flags, &pp, &g_dev);
    if (FAILED(hr)) {
        logf("host: CreateDevice failed 0x%08lx, retrying with software vertex processing", hr);
        flags = D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE | D3DCREATE_MULTITHREADED;
        hr = g_d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, g_hwnd, flags, &pp, &g_dev);
        if (FAILED(hr)) {
            logf("host: CreateDevice failed 0x%08lx", hr);
            return false;
        }
    }
    D3DADAPTER_IDENTIFIER9 id = {};
    g_d3d->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &id);
    g_intz = SUCCEEDED(g_d3d->CheckDeviceFormat(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DUSAGE_DEPTHSTENCIL,
                                                D3DRTYPE_TEXTURE, FMT_INTZ));
    logf("host: Direct3D 9 device, back buffer %dx%d on '%s'; INTZ depth textures %s", g_bb_w, g_bb_h, id.Description,
         g_intz ? "supported" : "NOT supported (depth-buffer reads return zero)");
    return true;
}

IDirect3DDevice9 *dev() { return g_dev; }
bool intz() { return g_intz; }

void begin_scene()
{
    if (!g_in_scene && g_dev) {
        g_dev->BeginScene();
        g_in_scene = true;
    }
}

void end_scene()
{
    if (g_in_scene && g_dev) {
        g_dev->EndScene();
        g_in_scene = false;
    }
}

HRESULT present()
{
    if (!g_dev) return D3D_OK;
    begin_scene();
    g_dev->EndScene();
    g_in_scene = false;
    RECT src = {0, 0, g_frame_w ? g_frame_w : g_bb_w, g_frame_h ? g_frame_h : g_bb_h};
    return g_dev->Present(&src, nullptr, nullptr, nullptr); // stretched to the client area (1:1 unless it outgrew the monitors)
}

// V-SYNC from the menu: the present interval is a creation parameter, applied by a device Reset at the next Swap.
void set_vsync(bool on)
{
    g_pp.PresentationInterval = on ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;
    xd3d::request_device_reset();
    logf("host: v-sync %s", on ? "on (Present waits for the monitor's refresh)" : "off");
}

bool reset_device()
{
    end_scene();
    D3DPRESENT_PARAMETERS pp = g_pp;
    HRESULT hr = g_dev->Reset(&pp);
    if (FAILED(hr)) logf("host: Reset failed 0x%08lx", hr);
    return SUCCEEDED(hr);
}

} // namespace host3d
