// rawkbd.cpp - 通过 Raw Input 判定按键来源设备（绕过键盘集合独占）
//
// 背景：Windows 8+ 对标准键盘/鼠标 HID 集合做独占保护，
//       普通进程 CreateFile(GENERIC_READ) 返回 ERROR_ACCESS_DENIED(5)。
//       改用 RegisterRawInputDevices + RIDEV_INPUTSINK：无需管理员，
//       且 RAWINPUTHEADER.hDevice 可反查设备接口路径，从而定位 VID/PID。
//
// 编译：cl /std:c++17 /utf-8 /EHsc /MT /O2 rawkbd.cpp /link user32.lib hid.lib
// 用法：rawkbd.exe [--seconds N]

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <hidsdi.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <map>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "hid.lib")

namespace {

std::map<HANDLE, std::string> g_deviceTags;
std::map<std::string, int> g_hits;
int g_total = 0;

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::string TagFromPath(const std::wstring& path) {
    std::string p = Narrow(path);
    size_t pos = p.find("vid_");
    if (pos == std::string::npos) pos = p.find("VID_");
    if (pos == std::string::npos) return "<unknown>";
    size_t end = p.find("#{", pos);
    std::string seg = p.substr(pos, end == std::string::npos ? p.size() - pos : end - pos);
    for (char& c : seg) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    return seg;
}

std::string DeviceTag(HANDLE dev) {
    auto it = g_deviceTags.find(dev);
    if (it != g_deviceTags.end()) return it->second;

    UINT size = 0;
    GetRawInputDeviceInfoW(dev, RIDI_DEVICENAME, nullptr, &size);
    std::string tag = "<unknown>";
    if (size > 1) {
        std::wstring name(size, L'\0');
        if (GetRawInputDeviceInfoW(dev, RIDI_DEVICENAME, name.data(), &size) != static_cast<UINT>(-1)) {
            tag = TagFromPath(name);
        }
    }
    g_deviceTags[dev] = tag;
    return tag;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_INPUT: {
            UINT size = 0;
            GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, nullptr, &size,
                            sizeof(RAWINPUTHEADER));
            if (size == 0) break;

            std::vector<BYTE> buf(size);
            if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, buf.data(), &size,
                                sizeof(RAWINPUTHEADER)) != size) {
                break;
            }
            const RAWINPUT* raw = reinterpret_cast<const RAWINPUT*>(buf.data());
            if (raw->header.dwType != RIM_TYPEKEYBOARD) break;

            const RAWKEYBOARD& kb = raw->data.keyboard;
            const bool released = (kb.Flags & RI_KEY_BREAK) != 0;
            const std::string tag = DeviceTag(raw->header.hDevice);

            if (!released) {
                ++g_total;
                ++g_hits[tag];
                std::printf("[%s] 按下 VKey=0x%02X Scan=0x%02X\n", tag.c_str(),
                            kb.VKey & 0xFF, kb.MakeCode);
                std::fflush(stdout);
            }
            break;
        }
        case WM_TIMER:
            PostQuitMessage(0);
            break;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);

    int seconds = 60;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--seconds" && i + 1 < argc) {
            seconds = std::atoi(argv[i + 1]);
        }
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"RawKbdProbeWnd";
    if (!RegisterClassExW(&wc)) {
        std::printf("注册窗口类失败，错误码 %lu\n", GetLastError());
        return 1;
    }

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE,
                                nullptr, wc.hInstance, nullptr);
    if (hwnd == nullptr) {
        std::printf("创建窗口失败，错误码 %lu\n", GetLastError());
        return 1;
    }

    RAWINPUTDEVICE rid{};
    rid.usUsagePage = 0x01;
    rid.usUsage = 0x06;
    rid.dwFlags = RIDEV_INPUTSINK;
    rid.hwndTarget = hwnd;
    if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) {
        std::printf("注册 Raw Input 失败，错误码 %lu\n", GetLastError());
        return 1;
    }

    std::printf("Raw Input 已注册，监听 %d 秒（后台有效，无需焦点）。\n", seconds);
    std::printf("请连续敲击 X87 上的 A / S / D / 空格。\n\n");
    Beep(1200, 200);
    SetTimer(hwnd, 1, static_cast<UINT>(seconds) * 1000, nullptr);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    Beep(800, 300);
    std::printf("\n================ 结果 ================\n");
    std::printf("共捕获 %d 次按键。\n", g_total);
    for (const auto& kv : g_hits) {
        std::printf("  %-46s %d 次\n", kv.first.c_str(), kv.second);
    }
    if (g_total == 0) {
        std::printf("未捕获任何按键：键盘可能未开机或未唤醒。\n");
    } else {
        std::printf("\n按键数最多的设备即为当前实际在用的键盘。\n");
    }
    return 0;
}
