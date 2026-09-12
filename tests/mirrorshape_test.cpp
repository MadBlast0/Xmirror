// MirrorShape against a real d3d11videosink window, including a rotation in the
// middle of the stream.
//
// Needs an interactive desktop session with Direct3D 11. Exit code 0 = pass.
#include "mirrorshape.h"

#include <gst/gst.h>
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>

namespace {

int failures = 0;
std::atomic<int> toggles{0};
std::atomic<bool> lastToggle{true};
HWND sinkWindow = nullptr;

constexpr UINT kLockAspectCommand = 0x0100;  // as in mirrorshape.cpp

void expect(bool ok, const char *what) {
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

BOOL CALLBACK findSinkWindow(HWND hwnd, LPARAM) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(hwnd)) return TRUE;
    wchar_t title[256];
    GetWindowTextW(hwnd, title, 256);
    const std::wstring text(title);
    if (text.find(L"Direct") != std::wstring::npos && text.find(L"enderer") != std::wstring::npos) {
        sinkWindow = hwnd;
        return FALSE;
    }
    return TRUE;
}

double clientAspect(HWND hwnd, int *width = nullptr, int *height = nullptr) {
    RECT client{};
    GetClientRect(hwnd, &client);
    if (width) *width = client.right;
    if (height) *height = client.bottom;
    return client.bottom ? double(client.right) / double(client.bottom) : 0.0;
}

SIZE frameSize(HWND hwnd) {
    RECT window{};
    RECT client{};
    GetWindowRect(hwnd, &window);
    GetClientRect(hwnd, &client);
    return {(window.right - window.left) - client.right, (window.bottom - window.top) - client.bottom};
}

double proposedAspect(HWND hwnd, const RECT &rect) {
    const SIZE frame = frameSize(hwnd);
    return double((rect.right - rect.left) - frame.cx) / double((rect.bottom - rect.top) - frame.cy);
}

}  // namespace

int main(int argc, char **argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    gst_init(&argc, &argv);

    const double landscape = 2388.0 / 1668.0;  // an 11-inch iPad
    const double portrait = 1668.0 / 2388.0;

    GError *error = nullptr;
    GstElement *pipeline = gst_parse_launch(
        "videotestsrc is-live=true pattern=white ! "
        "capsfilter name=shape caps=video/x-raw,width=2388,height=1668,framerate=30/1 ! "
        "videoconvert ! d3d11videosink",
        &error);
    if (!pipeline) {
        std::printf("pipeline could not be created: %s\n", error ? error->message : "unknown error");
        return 2;
    }
    GstElement *shape = gst_bin_get_by_name(GST_BIN(pipeline), "shape");
    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    for (int i = 0; i < 50 && !sinkWindow; ++i) {
        Sleep(100);
        EnumWindows(findSinkWindow, 0);
    }
    expect(sinkWindow != nullptr, "d3d11videosink window found");
    if (!sinkWindow) return 1;
    const HWND hwnd = sinkWindow;

    // Landscape: install and fit.
    MirrorShape::install(hwnd, true, [](bool locked) {
        lastToggle = locked;
        ++toggles;
    });
    MirrorShape::setAspect(landscape);
    MirrorShape::fit(hwnd, 2388, 1668);
    Sleep(600);
    expect(std::fabs(clientAspect(hwnd) - landscape) < 0.01, "landscape: window matches the picture");

    // Rotate mid-stream. The sink keeps its window shape; the fit corrects it.
    GstCaps *rotated = gst_caps_from_string("video/x-raw,width=1668,height=2388,framerate=30/1");
    g_object_set(shape, "caps", rotated, nullptr);
    gst_caps_unref(rotated);
    Sleep(800);
    int widthBefore = 0;
    int heightBefore = 0;
    const double unfitted = clientAspect(hwnd, &widthBefore, &heightBefore);
    expect(std::fabs(unfitted - portrait) > 0.1, "rotation alone leaves the old window shape");

    MirrorShape::setAspect(portrait);
    MirrorShape::fit(hwnd, 1668, 2388);
    Sleep(600);
    int width = 0;
    int height = 0;
    expect(std::fabs(clientAspect(hwnd, &width, &height) - portrait) < 0.01,
           "portrait: window matches the picture after rotation");
    expect(height <= heightBefore + 2, "portrait: height kept, or reduced only to fit the monitor");

    // Interactive resizing is held to the picture's shape.
    RECT window{};
    GetWindowRect(hwnd, &window);

    RECT edge = window;
    edge.right += 240;
    SendMessageW(hwnd, WM_SIZING, WMSZ_RIGHT, reinterpret_cast<LPARAM>(&edge));
    expect(std::fabs(proposedAspect(hwnd, edge) - portrait) < 0.01, "locked: edge drag keeps the shape");
    expect(edge.left == window.left && edge.top == window.top, "locked: edge drag keeps the far corner");

    RECT corner = window;
    corner.left -= 300;
    corner.top -= 40;
    SendMessageW(hwnd, WM_SIZING, WMSZ_TOPLEFT, reinterpret_cast<LPARAM>(&corner));
    expect(std::fabs(proposedAspect(hwnd, corner) - portrait) < 0.01, "locked: corner drag keeps the shape");
    expect(corner.right == window.right && corner.bottom == window.bottom,
           "locked: corner drag anchors the opposite corner");

    // The title-bar menu toggle.
    const HMENU menu = GetSystemMenu(hwnd, FALSE);
    expect((GetMenuState(menu, kLockAspectCommand, MF_BYCOMMAND) & MF_CHECKED) != 0,
           "menu item present and checked");
    SendMessageW(hwnd, WM_SYSCOMMAND, kLockAspectCommand, 0);
    expect(toggles == 1 && !lastToggle, "menu toggle reports unlocked");
    expect((GetMenuState(menu, kLockAspectCommand, MF_BYCOMMAND) & MF_CHECKED) == 0,
           "menu item unchecked");

    RECT free = window;
    free.right += 240;
    SendMessageW(hwnd, WM_SIZING, WMSZ_RIGHT, reinterpret_cast<LPARAM>(&free));
    expect(free.right == window.right + 240 && free.bottom == window.bottom,
           "unlocked: dragging is not constrained");

    const int menuItems = GetMenuItemCount(menu);
    MirrorShape::install(hwnd, false, nullptr);
    expect(GetMenuItemCount(menu) == menuItems, "installing again adds no second menu item");

    MirrorShape::release(hwnd);
    RECT released = window;
    released.right += 240;
    SendMessageW(hwnd, WM_SIZING, WMSZ_RIGHT, reinterpret_cast<LPARAM>(&released));
    expect(released.right == window.right + 240, "released: the window's own procedure is back");

    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(shape);
    gst_object_unref(pipeline);

    std::printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
