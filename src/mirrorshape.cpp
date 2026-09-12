#include "mirrorshape.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>

namespace {

// System-menu command ids must sit below 0xF000 with the low four bits clear.
constexpr UINT kLockAspectCommand = 0x0100;

// Read on the window's thread, written on the GUI thread.
std::atomic<double> g_aspect{0.0};  // width / height; 0 = unknown
std::atomic<bool> g_locked{true};
std::atomic<WNDPROC> g_originalProc{nullptr};
std::atomic<MirrorShape::LockToggled> g_onToggled{nullptr};

// Frame and caption size: window size minus client size.
SIZE nonClientSize(HWND hwnd) {
    RECT window{};
    RECT client{};
    GetWindowRect(hwnd, &window);
    GetClientRect(hwnd, &client);
    return {(window.right - window.left) - client.right,
            (window.bottom - window.top) - client.bottom};
}

void setMenuCheck(HWND hwnd, bool locked) {
    if (HMENU menu = GetSystemMenu(hwnd, FALSE)) {
        CheckMenuItem(menu, kLockAspectCommand,
                      MF_BYCOMMAND | (locked ? MF_CHECKED : MF_UNCHECKED));
    }
}

// Adjusts a WM_SIZING rectangle so the client area keeps `aspect`. The edge
// being dragged decides which dimension leads; a corner follows the larger
// change. The edges opposite the drag stay put.
void constrainSizingRect(HWND hwnd, WPARAM edge, RECT *rect, double aspect) {
    const SIZE frame = nonClientSize(hwnd);
    long width = std::max(160L, (rect->right - rect->left) - frame.cx);
    long height = std::max(90L, (rect->bottom - rect->top) - frame.cy);

    switch (edge) {
        case WMSZ_LEFT:
        case WMSZ_RIGHT:
            height = std::lround(width / aspect);
            break;
        case WMSZ_TOP:
        case WMSZ_BOTTOM:
            width = std::lround(height * aspect);
            break;
        default: {
            const long widthFromHeight = std::lround(height * aspect);
            if (widthFromHeight >= width) {
                width = widthFromHeight;
            } else {
                height = std::lround(width / aspect);
            }
            break;
        }
    }

    const bool movesLeft = edge == WMSZ_LEFT || edge == WMSZ_TOPLEFT || edge == WMSZ_BOTTOMLEFT;
    const bool movesTop = edge == WMSZ_TOP || edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT;
    if (movesLeft) {
        rect->left = rect->right - (width + frame.cx);
    } else {
        rect->right = rect->left + width + frame.cx;
    }
    if (movesTop) {
        rect->top = rect->bottom - (height + frame.cy);
    } else {
        rect->bottom = rect->top + height + frame.cy;
    }
}

LRESULT CALLBACK MirrorWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    const WNDPROC original = g_originalProc.load();
    const auto passOn = [&]() {
        return original ? CallWindowProcW(original, hwnd, message, wParam, lParam)
                        : DefWindowProcW(hwnd, message, wParam, lParam);
    };

    switch (message) {
        case WM_SIZING: {
            const LRESULT result = passOn();
            const double aspect = g_aspect.load();
            if (!g_locked.load() || aspect <= 0.0) return result;
            constrainSizingRect(hwnd, wParam, reinterpret_cast<RECT *>(lParam), aspect);
            return TRUE;
        }

        case WM_SYSCOMMAND:
            if ((wParam & 0xFFF0) == kLockAspectCommand) {
                const bool locked = !g_locked.load();
                g_locked.store(locked);
                setMenuCheck(hwnd, locked);
                if (MirrorShape::LockToggled notify = g_onToggled.load()) notify(locked);
                return 0;
            }
            break;

        default:
            break;
    }
    return passOn();
}

}  // namespace

void MirrorShape::install(void *window, bool locked, LockToggled onToggled) {
    const HWND hwnd = static_cast<HWND>(window);
    if (!hwnd || !IsWindow(hwnd)) return;

    g_locked.store(locked);
    g_onToggled.store(onToggled);

    if (GetWindowLongPtrW(hwnd, GWLP_WNDPROC) != reinterpret_cast<LONG_PTR>(&MirrorWindowProc)) {
        const LONG_PTR previous =
            SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&MirrorWindowProc));
        g_originalProc.store(reinterpret_cast<WNDPROC>(previous));
    }

    // The title bar's own menu (right-click the title bar, or Alt+Space) is the
    // part of a GStreamer-drawn window that can be extended without taking over
    // its painting. Inserted above Close, followed by a separator.
    if (HMENU menu = GetSystemMenu(hwnd, FALSE)) {
        if (GetMenuState(menu, kLockAspectCommand, MF_BYCOMMAND) == UINT(-1)) {
            InsertMenuW(menu, SC_CLOSE, MF_BYCOMMAND | MF_STRING | (locked ? MF_CHECKED : 0),
                        kLockAspectCommand, L"Lock aspect ratio");
            InsertMenuW(menu, SC_CLOSE, MF_BYCOMMAND | MF_SEPARATOR, 0, nullptr);
        } else {
            setMenuCheck(hwnd, locked);
        }
    }
}

void MirrorShape::release(void *window) {
    const HWND hwnd = static_cast<HWND>(window);
    if (hwnd && IsWindow(hwnd) &&
        GetWindowLongPtrW(hwnd, GWLP_WNDPROC) == reinterpret_cast<LONG_PTR>(&MirrorWindowProc)) {
        if (WNDPROC original = g_originalProc.load()) {
            SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(original));
        }
    }
    g_originalProc.store(nullptr);
    g_aspect.store(0.0);
}

void MirrorShape::setAspect(double aspect) {
    g_aspect.store(aspect > 0.0 ? aspect : 0.0);
}

void MirrorShape::setLocked(void *window, bool locked) {
    g_locked.store(locked);
    const HWND hwnd = static_cast<HWND>(window);
    if (hwnd && IsWindow(hwnd)) setMenuCheck(hwnd, locked);
}

bool MirrorShape::fit(void *window, int pictureWidth, int pictureHeight) {
    const HWND hwnd = static_cast<HWND>(window);
    if (!hwnd || !IsWindow(hwnd) || pictureWidth <= 0 || pictureHeight <= 0) return false;

    // Minimised, maximised or fullscreen (the sink drops the caption for
    // Alt+Enter): a size the user chose, so leave it.
    const LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    if (IsIconic(hwnd) || IsZoomed(hwnd) || !(style & WS_CAPTION)) return false;

    RECT bounds{};
    RECT client{};
    GetWindowRect(hwnd, &bounds);
    GetClientRect(hwnd, &client);
    const SIZE frame = nonClientSize(hwnd);
    const double aspect = double(pictureWidth) / double(pictureHeight);

    double height = client.bottom > 0 ? double(client.bottom) : double(pictureHeight);
    double width = height * aspect;

    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor);
    const RECT &work = monitor.rcWork;
    const double maxWidth = double((work.right - work.left) - frame.cx);
    const double maxHeight = double((work.bottom - work.top) - frame.cy);
    const double scale = std::min({1.0, maxWidth / width, maxHeight / height});
    width *= scale;
    height *= scale;

    const int newWidth = int(std::lround(width)) + frame.cx;
    const int newHeight = int(std::lround(height)) + frame.cy;

    // Around the current centre, then kept on the monitor.
    const int centreX = (bounds.left + bounds.right) / 2;
    const int centreY = (bounds.top + bounds.bottom) / 2;
    const int x = std::clamp(centreX - newWidth / 2, int(work.left),
                             std::max(int(work.left), int(work.right) - newWidth));
    const int y = std::clamp(centreY - newHeight / 2, int(work.top),
                             std::max(int(work.top), int(work.bottom) - newHeight));

    if (x == bounds.left && y == bounds.top && newWidth == bounds.right - bounds.left &&
        newHeight == bounds.bottom - bounds.top) {
        return false;
    }

    // Asynchronous: the window's thread belongs to GStreamer, and waiting on it
    // from the GUI thread could stall the settings window.
    SetWindowPos(hwnd, nullptr, x, y, newWidth, newHeight,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
    return true;
}
