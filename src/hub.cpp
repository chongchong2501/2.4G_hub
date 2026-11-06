// hub.cpp - P1 骨架：单实例 / 托盘 / 简洁面板 / 便携配置 / 自启 / 退出序列
//
// 约束对应：
//   C1 零第三方依赖  —— 仅 Windows SDK（user32/gdi32/d2d1/dwrite/shell32/advapi32）
//   C2 三种形态      —— 面板窗口 + 托盘常驻（调试版 P5 接入）
//   C3 托盘交互      —— 电量角标、左键开面板、右键菜单
//   C4 低 CPU        —— 纯消息驱动，无动画、无轮询忙等
//   C5 开机自启      —— HKCU\...\Run + 启动时路径自校正
//   C6 不写日志      —— 本文件不产生任何日志文件
//   C7 真正退出      —— 显式清理序列
//   C8 简洁美观      —— Direct2D 绘制，按 §4.5 色板与间距
//
// 编译：cl /std:c++17 /utf-8 /EHsc /MT /O2 hub.cpp
//       /link user32.lib gdi32.lib shell32.lib advapi32.lib ole32.lib d2d1.lib dwrite.lib

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <dwmapi.h>

// 必须在 d2d1.h 之前取消宏：否则 ID2D1RenderTarget::DrawText 的声明
// 会被 windows.h 的 UNICODE 宏改写成 DrawTextW
#ifdef DrawText
#undef DrawText
#endif

#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>

#include <cstdio>
#include <cstdint>
#include <cmath>
#include <cwchar>
#include <string>
#include <vector>

#include "device.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

// windows.h 将 DrawText 宏定义为 DrawTextW，会与 ID2D1RenderTarget::DrawText 冲突
#ifdef DrawText
#undef DrawText
#endif

namespace {

// ---------- 常量 ----------

constexpr wchar_t kPanelClass[] = L"MchoseEweadnHubPanel";
constexpr wchar_t kAppTitle[] = L"Mchose EWEADN Hub";
constexpr wchar_t kMutexName[] = L"Local\\MchoseEweadnHub_SingleInstance";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"MchoseEweadnHub";

constexpr UINT WM_TRAYICON = WM_APP + 1;
constexpr UINT WM_ACTIVATE_HUB = WM_APP + 2;
constexpr UINT WM_DEVICE_UPDATE = WM_APP + 3;

constexpr int IDM_OPEN = 1001;
constexpr int IDM_AUTOSTART = 1002;
constexpr int IDM_DEBUG = 1003;
constexpr int IDM_EXIT = 1004;
constexpr int IDM_DPI_BASE = 2000;
constexpr int IDM_RATE_BASE = 2100;

const int kRateValues[6] = {125, 500, 1000, 2000, 4000, 8000};

// §4.5 色板
const D2D1_COLOR_F kBg = D2D1::ColorF(0x1B1D21);
const D2D1_COLOR_F kCard = D2D1::ColorF(0x24262B);
const D2D1_COLOR_F kLine = D2D1::ColorF(0x34373D);
const D2D1_COLOR_F kText = D2D1::ColorF(0xE6E8EB);
const D2D1_COLOR_F kTextDim = D2D1::ColorF(0x9AA0A6);
const D2D1_COLOR_F kAccent = D2D1::ColorF(0x4C8DFF);
const D2D1_COLOR_F kOk = D2D1::ColorF(0x3FB950);
const D2D1_COLOR_F kWarn = D2D1::ColorF(0xD29922);
const D2D1_COLOR_F kBad = D2D1::ColorF(0xF85149);

// ---------- 全局状态 ----------

struct AppState {
    HINSTANCE inst = nullptr;
    HWND panel = nullptr;
    HICON trayIcon = nullptr;
    UINT taskbarCreated = 0;
    bool exiting = false;
    bool panelVisible = false;
    bool sizeAdjusted = false;
    UINT dpi = 96;

    // 设备数据（由工作线程读取，UI 线程消费）
    int mouseBattery = -1;
    int kbBattery = -1;
    bool mouseConnected = false;
    bool kbConnected = false;
    DeviceReading reading;
    MouseConfig mouseCfg;
    KeyboardLed kbLed;
    HANDLE devThread = nullptr;
    HANDLE wakeEvent = nullptr;
    HANDLE stopEvent = nullptr;

    // 配置
    int winX = -1;
    int winY = -1;
    int winW = 760;
    int winH = 360;
    bool autostart = false;
    int lowBattery = 15;
    std::wstring configDir;
    std::wstring configFile;
    bool portable = true;  // false 表示已降级到 LOCALAPPDATA

    ID2D1Factory* d2d = nullptr;
    ID2D1HwndRenderTarget* rt = nullptr;
    IDWriteFactory* dw = nullptr;
    IDWriteTextFormat* fmtTitle = nullptr;
    IDWriteTextFormat* fmtValue = nullptr;
    IDWriteTextFormat* fmtSmall = nullptr;
    IDWriteTextFormat* fmtHead = nullptr;
    IDWriteTextFormat* fmtWheelBig = nullptr;
    IDWriteTextFormat* fmtWheelMid = nullptr;
    IDWriteTextFormat* fmtWheelSmall = nullptr;
};

AppState g;

float S(float v) { return v * static_cast<float>(g.dpi) / 96.0f; }

int RateHz(int index) {
    static const int kRates[] = {125, 500, 1000, 2000, 4000, 8000};
    return (index >= 0 && index < 6) ? kRates[index] : 0;
}

// ---------- 便携存储 ----------

bool EnsureDir(const std::wstring& path) {
    return SHCreateDirectoryExW(nullptr, path.c_str(), nullptr) == ERROR_SUCCESS ||
           GetLastError() == ERROR_ALREADY_EXISTS;
}

std::wstring ExeDir() {
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p(buf);
    size_t pos = p.find_last_of(L'\\');
    return pos == std::wstring::npos ? p : p.substr(0, pos);
}

std::wstring ExePath() {
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return std::wstring(buf);
}

// 便携优先：exe 同目录可写则用 config\，否则降级 %LOCALAPPDATA%
void ResolveConfigDir() {
    std::wstring local = ExeDir() + L"\\config";
    if (EnsureDir(local)) {
        std::wstring probe = local + L"\\.write_test";
        HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_TEMPORARY, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
            DeleteFileW(probe.c_str());
            g.configDir = local;
            g.portable = true;
            g.configFile = local + L"\\settings.json";
            return;
        }
    }

    PWSTR roaming = nullptr;
    std::wstring fallback;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &roaming))) {
        fallback = std::wstring(roaming) + L"\\MchoseEweadnHub\\config";
        CoTaskMemFree(roaming);
    } else {
        fallback = ExeDir() + L"\\config";
    }
    EnsureDir(fallback);
    g.configDir = fallback;
    g.portable = false;
    g.configFile = fallback + L"\\settings.json";
}

int JsonInt(const std::wstring& s, const wchar_t* key, int def) {
    std::wstring k = std::wstring(L"\"") + key + L"\"";
    size_t p = s.find(k);
    if (p == std::wstring::npos) return def;
    p = s.find(L':', p + k.size());
    if (p == std::wstring::npos) return def;
    return static_cast<int>(wcstol(s.c_str() + p + 1, nullptr, 10));
}

bool JsonBool(const std::wstring& s, const wchar_t* key, bool def) {
    std::wstring k = std::wstring(L"\"") + key + L"\"";
    size_t p = s.find(k);
    if (p == std::wstring::npos) return def;
    p = s.find(L':', p + k.size());
    if (p == std::wstring::npos) return def;
    size_t v = s.find_first_not_of(L" \t\r\n", p + 1);
    if (v == std::wstring::npos) return def;
    if (s.compare(v, 4, L"true") == 0) return true;
    if (s.compare(v, 5, L"false") == 0) return false;
    return def;
}

void LoadConfig() {
    HANDLE h = CreateFileW(g.configFile.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;

    DWORD size = GetFileSize(h, nullptr);
    if (size == 0 || size > 64 * 1024) { CloseHandle(h); return; }

    std::string raw(size, '\0');
    DWORD read = 0;
    BOOL ok = ReadFile(h, raw.data(), size, &read, nullptr);
    CloseHandle(h);
    if (!ok || read == 0) return;

    int wlen = MultiByteToWideChar(CP_UTF8, 0, raw.data(), static_cast<int>(read), nullptr, 0);
    std::wstring text(static_cast<size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, raw.data(), static_cast<int>(read), text.data(), wlen);

    g.winX = JsonInt(text, L"x", g.winX);
    g.winY = JsonInt(text, L"y", g.winY);
    g.winW = JsonInt(text, L"w", g.winW);
    g.winH = JsonInt(text, L"h", g.winH);
    g.autostart = JsonBool(text, L"autostart", g.autostart);
    g.lowBattery = JsonInt(text, L"lowBattery", g.lowBattery);
}

bool WriteUtf8File(const std::wstring& path, const std::string& data) {
    std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
    CloseHandle(h);
    if (!ok) { DeleteFileW(tmp.c_str()); return false; }
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
}

void SaveConfig() {
    char buf[1024];
    std::snprintf(buf, sizeof(buf),
                  "{\n"
                  "  \"schemaVersion\": 1,\n"
                  "  \"window\": { \"x\": %d, \"y\": %d, \"w\": %d, \"h\": %d },\n"
                  "  \"autostart\": %s,\n"
                  "  \"lowBattery\": %d\n"
                  "}\n",
                  g.winX, g.winY, g.winW, g.winH, g.autostart ? "true" : "false", g.lowBattery);
    WriteUtf8File(g.configFile, std::string(buf));
}

// ---------- 开机自启 ----------

bool ReadRunValue(std::wstring* out) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
    wchar_t buf[1024] = {};
    DWORD size = sizeof(buf);
    DWORD type = 0;
    bool ok = RegQueryValueExW(key, kRunValue, nullptr, &type,
                               reinterpret_cast<LPBYTE>(buf), &size) == ERROR_SUCCESS &&
              type == REG_SZ;
    RegCloseKey(key);
    if (ok && out) *out = buf;
    return ok;
}

void ApplyAutoStart(bool enable) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr,
                        &key, nullptr) != ERROR_SUCCESS) {
        return;
    }
    if (enable) {
        std::wstring cmd = L"\"" + ExePath() + L"\" --tray";
        RegSetValueExW(key, kRunValue, 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(cmd.c_str()),
                       static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, kRunValue);
    }
    RegCloseKey(key);
}

// 绿色版会被移动：启动时校正注册表路径（FR-T11）
void FixAutoStartPath() {
    std::wstring current;
    if (!ReadRunValue(&current)) return;
    std::wstring expected = L"\"" + ExePath() + L"\" --tray";
    if (current != expected) ApplyAutoStart(true);
}

// ---------- 托盘图标（动态绘制，含电量角标） ----------

D2D1_COLOR_F BatteryColor(int level) {
    if (level < 0) return D2D1::ColorF(0x6B7076);
    if (level >= 50) return kOk;
    if (level >= 20) return kWarn;
    return kBad;
}

HICON MakeTrayIcon(int level) {
    const int sz = 32;
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = sz;
    bi.bV5Height = -sz;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;

    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS,
                                     &bits, nullptr, 0);
    HDC dc = CreateCompatibleDC(screen);
    HBITMAP old = static_cast<HBITMAP>(SelectObject(dc, color));
    ReleaseDC(nullptr, screen);

    auto* px = static_cast<uint32_t*>(bits);
    for (int i = 0; i < sz * sz; ++i) px[i] = 0;  // 全透明

    auto put = [&](int x, int y, uint8_t r, uint8_t gg, uint8_t b, uint8_t a) {
        if (x < 0 || y < 0 || x >= sz || y >= sz) return;
        uint32_t& d = px[y * sz + x];
        d = (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(r) << 16) |
            (static_cast<uint32_t>(gg) << 8) | b;
    };

    // 设备剪影：圆角方块
    for (int y = 5; y < 27; ++y) {
        for (int x = 4; x < 24; ++x) {
            int cx = (x < 8) ? 8 : (x > 19 ? 19 : x);
            int cy = (y < 9) ? 9 : (y > 22 ? 22 : y);
            int dx = x - cx;
            int dy = y - cy;
            if (dx * dx + dy * dy <= 12) put(x, y, 0xE6, 0xE8, 0xEB, 0xFF);
        }
    }

    D2D1_COLOR_F c = BatteryColor(level);
    uint8_t cr = static_cast<uint8_t>(c.r * 255);
    uint8_t cg = static_cast<uint8_t>(c.g * 255);
    uint8_t cb = static_cast<uint8_t>(c.b * 255);

    // 右下角电量圆点（5px）
    for (int y = 0; y < sz; ++y) {
        for (int x = 0; x < sz; ++x) {
            int dx = x - 25;
            int dy = y - 25;
            if (dx * dx + dy * dy <= 30) put(x, y, cr, cg, cb, 0xFF);
        }
    }

    SelectObject(dc, old);
    DeleteDC(dc);

    HBITMAP mask = CreateBitmap(sz, sz, 1, 1, nullptr);
    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmMask = mask;
    ii.hbmColor = color;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    return icon;
}

void UpdateTrayIcon() {
    HICON old = g.trayIcon;
    int level = g.mouseBattery;
    if (g.kbBattery >= 0 && (level < 0 || g.kbBattery < level)) level = g.kbBattery;
    g.trayIcon = MakeTrayIcon(level);

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g.panel;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = g.trayIcon;

    auto line = [](const wchar_t* name, int v, bool conn) -> std::wstring {
        if (!conn) return std::wstring(name) + L"　未连接";
        if (v < 0) return std::wstring(name) + L"　电量未知";
        return std::wstring(name) + L"　" + std::to_wstring(v) + L"%";
    };
    std::wstring tip = line(L"鼠标 A7 Pro", g.mouseBattery, g.mouseConnected) + L"\n" +
                       line(L"键盘 X87", g.kbBattery, g.kbConnected);
    wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);

    Shell_NotifyIconW(NIM_MODIFY, &nid);
    if (old != nullptr) DestroyIcon(old);
}

void AddTrayIcon() {
    if (g.trayIcon == nullptr) g.trayIcon = MakeTrayIcon(-1);
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g.panel;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = g.trayIcon;
    wcscpy_s(nid.szTip, L"Mchose EWEADN Hub");
    Shell_NotifyIconW(NIM_ADD, &nid);
}

void RemoveTrayIcon() {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g.panel;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

// ---------- Direct2D ----------

bool CreateTextFormats() {
    struct Spec {
        IDWriteTextFormat** dst;
        float size;
        DWRITE_FONT_WEIGHT weight;
        bool center;
    };
    const Spec specs[] = {
        {&g.fmtHead, 18.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, false},
        {&g.fmtTitle, 15.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, false},
        {&g.fmtValue, 28.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, false},
        {&g.fmtSmall, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, false},
        {&g.fmtWheelBig, 24.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, true},
        {&g.fmtWheelMid, 16.0f, DWRITE_FONT_WEIGHT_NORMAL, true},
        {&g.fmtWheelSmall, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, true},
    };
    for (const Spec& s : specs) {
        if (*s.dst != nullptr) { (*s.dst)->Release(); *s.dst = nullptr; }
        const float px = s.size * static_cast<float>(g.dpi) / 96.0f;
        HRESULT hr = g.dw->CreateTextFormat(L"Segoe UI", nullptr, s.weight,
                                            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                            px, L"zh-CN", s.dst);
        if (FAILED(hr)) return false;
        (*s.dst)->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        if (s.center) {
            (*s.dst)->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            (*s.dst)->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    }
    return true;
}

void DiscardDeviceResources() {
    if (g.rt != nullptr) { g.rt->Release(); g.rt = nullptr; }
}

bool CreateDeviceResources() {
    if (g.rt != nullptr) return true;
    RECT rc{};
    GetClientRect(g.panel, &rc);
    D2D1_SIZE_U size = D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top);
    if (size.width == 0 || size.height == 0) return false;
    HRESULT hr = g.d2d->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(),
        D2D1::HwndRenderTargetProperties(g.panel, size), &g.rt);
    if (SUCCEEDED(hr)) g.rt->SetDpi(96.0f, 96.0f);  // 坐标体系与物理像素一致
    return SUCCEEDED(hr);
}

void DrawTextAt(const wchar_t* text, IDWriteTextFormat* fmt, const D2D1_RECT_F& rc,
                const D2D1_COLOR_F& color) {
    ID2D1SolidColorBrush* brush = nullptr;
    if (SUCCEEDED(g.rt->CreateSolidColorBrush(color, &brush))) {
        g.rt->DrawText(text, static_cast<UINT32>(wcslen(text)), fmt, rc, brush,
                        D2D1_DRAW_TEXT_OPTIONS_CLIP);
        brush->Release();
    }
}

void FillRound(const D2D1_RECT_F& rc, float radius, const D2D1_COLOR_F& color) {
    ID2D1SolidColorBrush* brush = nullptr;
    if (SUCCEEDED(g.rt->CreateSolidColorBrush(color, &brush))) {
        g.rt->FillRoundedRectangle(
            D2D1::RoundedRect(rc, S(radius), S(radius)), brush);
        brush->Release();
    }
}

void DrawCard(float x, float y, float w, float h, const wchar_t* name, const wchar_t* model,
              const wchar_t* link, const wchar_t* valueOverride, int battery, bool connected) {
    D2D1_RECT_F card = D2D1::RectF(S(x), S(y), S(x + w), S(y + h));
    FillRound(card, 12, kCard);

    DrawTextAt(name, g.fmtTitle, D2D1::RectF(S(x + 16), S(y + 16), S(x + w - 16), S(y + 38)),
               kText);
    DrawTextAt(link, g.fmtSmall, D2D1::RectF(S(x + 16), S(y + 38), S(x + w - 16), S(y + 56)),
               kTextDim);

    wchar_t val[32] = L"--";
    if (valueOverride != nullptr) {
        wcscpy_s(val, valueOverride);
    } else if (!connected) {
        wcscpy_s(val, L"未连接");
    } else if (battery >= 0) {
        swprintf_s(val, L"%d%%", battery);
    } else {
        wcscpy_s(val, L"未知");
    }
    DrawTextAt(val, g.fmtValue, D2D1::RectF(S(x + 16), S(y + 70), S(x + w - 16), S(y + 112)),
               connected ? kText : kTextDim);

    DrawTextAt(model, g.fmtSmall, D2D1::RectF(S(x + 16), S(y + 112), S(x + w - 16), S(y + 130)),
               kTextDim);

    // 电量条
    float barW = w - 32;
    D2D1_RECT_F track = D2D1::RectF(S(x + 16), S(y + h - 32), S(x + 16 + barW), S(y + h - 26));
    FillRound(track, 3, kLine);
    if (connected && battery > 0) {
        float pct = battery > 100 ? 1.0f : battery / 100.0f;
        D2D1_RECT_F fill = D2D1::RectF(S(x + 16), S(y + h - 32),
                                       S(x + 16 + barW * pct), S(y + h - 26));
        FillRound(fill, 3, battery < 20 ? kBad : (battery < 50 ? kWarn : kAccent));
    }
}

// ---------- 视图与滚轮 ----------

enum class ViewMode { Main, MouseDetail, KbDetail };
ViewMode g_view = ViewMode::Main;

struct Wheel {
    float x = 0.0f;
    float y = 0.0f;
    float w = 220.0f;
    float h = 170.0f;
    int count = 0;
    int selected = 0;
};

Wheel g_dpiWheel;
Wheel g_rateWheel;
Wheel g_modeWheel;
Wheel g_brightWheel;
Wheel g_speedWheel;

// 灯效模式表（来自 E HUB i18n 的 huafenda 分组）
const int kLedModeValues[22] = {9999, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11,
                                12, 13, 14, 15, 16, 17, 18, 19, 254, 20};
const wchar_t* kLedModeNames[22] = {
    L"关闭灯效", L"静态恒亮", L"按压点亮", L"释放熄灭", L"繁星点点", L"漫天飞雪",
    L"百花争艳", L"动态呼吸", L"光谱循环", L"彩泉涌动", L"多彩纵横", L"随波逐流",
    L"峰回路转", L"一触即发", L"一石二鸟", L"涟漪扩散", L"川流不息", L"重峦叠嶂",
    L"斜风细雨", L"来回穿梭", L"心动时刻", L"自定义"};
constexpr int kLedModeCount = 22;

int LedModeIndex(int modeValue) {
    for (int i = 0; i < kLedModeCount; ++i) {
        if (kLedModeValues[i] == modeValue) return i;
    }
    return 1;
}
int g_dragWheel = -1;
int g_dragStartY = 0;
int g_dragStartSel = 0;
int g_pendingWrite = -1;  // 0 = DPI, 1 = Rate

D2D1_COLOR_F AlphaText(float alpha) {
    return D2D1::ColorF(0.902f, 0.910f, 0.922f, alpha);
}

// 时钟式滚轮：中间选中项最大最亮，上下各两行逐级缩小变淡
void DrawWheel(const Wheel& wh, const std::vector<std::wstring>& labels, const wchar_t* title) {
    const D2D1_RECT_F card = D2D1::RectF(S(wh.x), S(wh.y), S(wh.x + wh.w), S(wh.y + wh.h));
    FillRound(card, 14, kCard);

    DrawTextAt(title, g.fmtSmall,
               D2D1::RectF(S(wh.x + 18), S(wh.y - 26), S(wh.x + wh.w - 18), S(wh.y - 6)), kTextDim);

    const float cy = wh.y + wh.h * 0.5f;
    const float itemH = 34.0f;

    // 选中行的底衬
    FillRound(D2D1::RectF(S(wh.x + 12), S(cy - itemH * 0.5f), S(wh.x + wh.w - 12), S(cy + itemH * 0.5f)),
              9, D2D1::ColorF(0.155f, 0.165f, 0.185f));

    for (int d = -2; d <= 2; ++d) {
        const int idx = wh.selected + d;
        if (idx < 0 || idx >= wh.count || idx >= static_cast<int>(labels.size())) continue;

        const float y = cy + d * itemH;
        const float alpha = (d == 0) ? 1.0f : ((d == 1 || d == -1) ? 0.42f : 0.16f);
        IDWriteTextFormat* fmt = (d == 0) ? g.fmtWheelBig
                                          : ((d == 1 || d == -1) ? g.fmtWheelMid : g.fmtWheelSmall);
        DrawTextAt(labels[idx].c_str(), fmt,
                   D2D1::RectF(S(wh.x), S(y - itemH * 0.5f), S(wh.x + wh.w), S(y + itemH * 0.5f)),
                   AlphaText(alpha));
    }
}

void DrawBackButton() {
    FillRound(D2D1::RectF(S(24), S(16), S(56), S(48)), 8, kCard);
    DrawTextAt(L"←", g.fmtHead, D2D1::RectF(S(28), S(18), S(56), S(46)), kText);
}

Wheel* WheelById(int id) {
    switch (id) {
        case 0: return &g_dpiWheel;
        case 1: return &g_rateWheel;
        case 2: return &g_modeWheel;
        case 3: return &g_brightWheel;
        default: return &g_speedWheel;
    }
}

void SyncWheels() {
    g_dpiWheel.x = 88.0f;
    g_dpiWheel.y = 112.0f;
    g_dpiWheel.w = 220.0f;
    g_dpiWheel.h = 170.0f;
    g_dpiWheel.count = (g.mouseCfg.ok && g.mouseCfg.dpiCount > 0) ? g.mouseCfg.dpiCount : 6;
    g_dpiWheel.selected = (g.mouseCfg.ok && g.mouseCfg.dpiIndex >= 0) ? g.mouseCfg.dpiIndex : 0;
    if (g_dpiWheel.selected >= g_dpiWheel.count) g_dpiWheel.selected = g_dpiWheel.count - 1;

    g_rateWheel.x = 452.0f;
    g_rateWheel.y = 112.0f;
    g_rateWheel.w = 220.0f;
    g_rateWheel.h = 170.0f;
    g_rateWheel.count = 6;
    g_rateWheel.selected = (g.mouseCfg.ok && g.mouseCfg.rateIndex >= 0) ? g.mouseCfg.rateIndex : 0;
}

// ---------- 三个视图 ----------

void OnPaintMain() {
    DrawTextAt(L"设备中心", g.fmtHead, D2D1::RectF(S(24), S(20), S(400), S(48)), kText);
    DrawTextAt(g.portable ? L"便携模式" : L"降级存储", g.fmtSmall,
               D2D1::RectF(S(560), S(26), S(720), S(44)), kTextDim);

    const float cardW = 350.0f;
    const float cardH = 200.0f;

    wchar_t mouseCfgText[64] = L"MCHOSE A7 Pro";
    if (g.mouseCfg.ok && g.mouseCfg.dpiIndex >= 0 && g.mouseCfg.dpiIndex < 6) {
        const int dpi = g.mouseCfg.dpiList[g.mouseCfg.dpiIndex];
        const int hz = RateHz(g.mouseCfg.rateIndex);
        if (hz > 0) {
            swprintf_s(mouseCfgText, L"%d DPI · %d Hz", dpi, hz);
        } else {
            swprintf_s(mouseCfgText, L"%d DPI", dpi);
        }
    } else if (!g.mouseConnected) {
        wcscpy_s(mouseCfgText, L"请插入接收器");
    }

    const bool mouseSleep = !g.mouseConnected && g.reading.mousePresent;
    DrawCard(24, 64, cardW, cardH, L"鼠标", mouseCfgText,
             g.mouseConnected ? L"2.4G · 已连接" : (mouseSleep ? L"移动鼠标唤醒" : L"未插入接收器"),
             g.mouseConnected ? nullptr : (mouseSleep ? L"无响应" : L"未连接"),
             g.mouseBattery, g.mouseConnected);

    const bool kbSleep = !g.kbConnected && g.reading.kbPresent;
    DrawCard(386, 64, cardW, cardH, L"键盘",
             g.kbConnected ? L"EWEADN X87" : (kbSleep ? L"敲击任意键唤醒" : L"请确认已开机"),
             g.kbConnected ? L"2.4G · 已连接" : (kbSleep ? L"休眠中" : L"未检测到设备"),
             g.kbConnected ? nullptr : (kbSleep ? L"休眠" : L"未连接"),
             g.kbBattery, g.kbConnected);

    DrawTextAt(L"×", g.fmtHead, D2D1::RectF(S(716), S(18), S(744), S(46)), kTextDim);
    DrawTextAt(L"点击设备卡片进入设置　·　Esc 关闭", g.fmtSmall,
               D2D1::RectF(S(24), S(296), S(736), S(316)), kTextDim);
}

void OnPaintMouseDetail() {
    DrawBackButton();
    DrawTextAt(L"鼠标 · MCHOSE A7 Pro", g.fmtHead,
               D2D1::RectF(S(70), S(20), S(480), S(46)), kText);

    wchar_t status[64];
    if (g.mouseConnected) {
        swprintf_s(status, L"%d%%　2.4G", g.mouseBattery);
    } else {
        wcscpy_s(status, L"离线");
    }
    DrawTextAt(status, g.fmtSmall, D2D1::RectF(S(600), S(24), S(736), S(44)), kTextDim);

    std::vector<std::wstring> dpiLabels;
    std::vector<std::wstring> rateLabels;
    for (int i = 0; i < g_dpiWheel.count; ++i) {
        wchar_t b[24];
        swprintf_s(b, L"%d", g.mouseCfg.ok ? g.mouseCfg.dpiList[i] : 0);
        dpiLabels.emplace_back(b);
    }
    for (int i = 0; i < 6; ++i) {
        wchar_t b[24];
        swprintf_s(b, L"%d", kRateValues[i]);
        rateLabels.emplace_back(b);
    }

    DrawWheel(g_dpiWheel, dpiLabels, L"DPI");
    DrawWheel(g_rateWheel, rateLabels, L"回报率（Hz）");

    DrawTextAt(L"上下滚动选择　·　松开后自动应用", g.fmtSmall,
               D2D1::RectF(S(24), S(300), S(736), S(320)), kTextDim);
}

void SyncKbWheels() {
    g_modeWheel.x = 24.0f;
    g_modeWheel.y = 108.0f;
    g_modeWheel.w = 300.0f;
    g_modeWheel.h = 190.0f;
    g_modeWheel.count = kLedModeCount;
    g_modeWheel.selected = LedModeIndex(g.kbLed.ok ? g.kbLed.mode : 1);

    g_brightWheel.x = 350.0f;
    g_brightWheel.y = 108.0f;
    g_brightWheel.w = 150.0f;
    g_brightWheel.h = 190.0f;
    g_brightWheel.count = 6;
    g_brightWheel.selected = g.kbLed.ok ? g.kbLed.brightness : 0;
    if (g_brightWheel.selected < 0) g_brightWheel.selected = 0;
    if (g_brightWheel.selected > 5) g_brightWheel.selected = 5;

    g_speedWheel.x = 524.0f;
    g_speedWheel.y = 108.0f;
    g_speedWheel.w = 150.0f;
    g_speedWheel.h = 190.0f;
    g_speedWheel.count = 6;
    g_speedWheel.selected = g.kbLed.ok ? g.kbLed.speed : 0;
    if (g_speedWheel.selected < 0) g_speedWheel.selected = 0;
    if (g_speedWheel.selected > 5) g_speedWheel.selected = 5;
}

void OnPaintKbDetail() {
    DrawBackButton();
    DrawTextAt(L"键盘 · EWEADN X87", g.fmtHead,
               D2D1::RectF(S(70), S(20), S(480), S(46)), kText);

    wchar_t status[64];
    if (g.kbConnected) {
        swprintf_s(status, L"%d%%　2.4G", g.kbBattery);
    } else {
        wcscpy_s(status, L"离线");
    }
    DrawTextAt(status, g.fmtSmall, D2D1::RectF(S(600), S(24), S(736), S(44)), kTextDim);

    std::vector<std::wstring> modeLabels;
    for (int i = 0; i < kLedModeCount; ++i) modeLabels.emplace_back(kLedModeNames[i]);

    std::vector<std::wstring> numLabels;
    for (int i = 0; i <= 5; ++i) {
        wchar_t b[8];
        swprintf_s(b, L"%d", i);
        numLabels.emplace_back(b);
    }

    DrawWheel(g_modeWheel, modeLabels, L"灯效");
    DrawWheel(g_brightWheel, numLabels, L"亮度");
    DrawWheel(g_speedWheel, numLabels, L"速度");

    DrawTextAt(L"上下滚动选择　·　松开后自动应用　·　改键待开发", g.fmtSmall,
               D2D1::RectF(S(24), S(308), S(736), S(328)), kTextDim);
}

void OnPaint() {
    if (g.fmtHead == nullptr) CreateTextFormats();
    if (!CreateDeviceResources()) return;

    g.rt->BeginDraw();
    g.rt->Clear(kBg);

    switch (g_view) {
        case ViewMode::MouseDetail:
            OnPaintMouseDetail();
            break;
        case ViewMode::KbDetail:
            OnPaintKbDetail();
            break;
        default:
            OnPaintMain();
            break;
    }

    HRESULT hr = g.rt->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) DiscardDeviceResources();
}

// ---------- 托盘菜单 ----------

void ShowTrayMenu() {
    POINT pt{};
    GetCursorPos(&pt);

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, IDM_OPEN, L"打开面板");
    AppendMenuW(menu, MF_STRING | (g.autostart ? MF_CHECKED : MF_UNCHECKED), IDM_AUTOSTART,
                L"开机自启");
    AppendMenuW(menu, MF_STRING, IDM_DEBUG, L"完整调试版（P5）");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"退出");

    SetForegroundWindow(g.panel);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, g.panel, nullptr);
    DestroyMenu(menu);
}

void ShowPanel() {
    if (!IsWindowVisible(g.panel)) {
        ShowWindow(g.panel, SW_SHOW);
        g.panelVisible = true;
    } else {
        SetForegroundWindow(g.panel);
    }
    UpdateWindow(g.panel);
}

void HidePanel() {
    ShowWindow(g.panel, SW_HIDE);
    g.panelVisible = false;
}

// ---------- 设备工作线程 ----------

DWORD WINAPI DeviceThreadProc(LPVOID) {
    HANDLE waits[2] = {g.wakeEvent, g.stopEvent};
    for (;;) {
        g.reading = ReadDevices();
        g.mouseCfg = ReadMouseConfig();
        g.kbLed = ReadKeyboardLed();
        if (g.panel != nullptr) PostMessageW(g.panel, WM_DEVICE_UPDATE, 0, 0);

        // 60 秒周期；唤醒事件可提前触发，停止事件立即退出
        DWORD w = WaitForMultipleObjects(2, waits, FALSE, 60000);
        if (w == WAIT_OBJECT_0 + 1) break;
        if (w == WAIT_OBJECT_0) ResetEvent(g.wakeEvent);
    }
    return 0;
}

// ---------- 窗口过程 ----------

void OnTrayMessage(LPARAM lp) {
    switch (LOWORD(lp)) {
        case WM_LBUTTONUP:
            ShowPanel();
            break;
        case WM_RBUTTONUP:
            ShowTrayMenu();
            break;
        default:
            break;
    }
}

LRESULT CALLBACK PanelProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE:
            g.panel = hwnd;
            return 0;

        case WM_PAINT: {
            PAINTSTRUCT ps{};
            BeginPaint(hwnd, &ps);

            // 首次绘制时窗口已完成显示器关联，此时 DPI 才可信
            if (!g.sizeAdjusted) {
                g.sizeAdjusted = true;
                UINT dpi = GetDpiForWindow(hwnd);
                if (dpi > 0) g.dpi = dpi;
                const int wantW = MulDiv(g.winW, static_cast<int>(g.dpi), 96);
                const int wantH = MulDiv(g.winH, static_cast<int>(g.dpi), 96);
                RECT cur{};
                GetWindowRect(hwnd, &cur);
                if ((cur.right - cur.left) != wantW || (cur.bottom - cur.top) != wantH) {
                    RECT work{};
                    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
                    SetWindowPos(hwnd, nullptr,
                                 work.left + ((work.right - work.left) - wantW) / 2,
                                 work.top + ((work.bottom - work.top) - wantH) / 2,
                                 wantW, wantH, SWP_NOZORDER | SWP_NOACTIVATE);
                    EndPaint(hwnd, &ps);
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
            }

            OnPaint();
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_SIZE:
            if (g.rt != nullptr) {
                g.rt->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
            }
            return 0;

        case WM_DPICHANGED: {
            g.dpi = HIWORD(wp);
            // 字号依赖 DPI，置空后下一帧重建
            auto drop = [](IDWriteTextFormat*& f) {
                if (f != nullptr) { f->Release(); f = nullptr; }
            };
            drop(g.fmtHead);
            drop(g.fmtTitle);
            drop(g.fmtValue);
            drop(g.fmtSmall);
            drop(g.fmtWheelBig);
            drop(g.fmtWheelMid);
            drop(g.fmtWheelSmall);
            DiscardDeviceResources();
            const RECT* prc = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd, nullptr, prc->left, prc->top,
                         prc->right - prc->left, prc->bottom - prc->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_NCHITTEST: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &pt);
            // 顶部 56px 可拖动；右上角关闭按钮除外
            if (pt.y < static_cast<LONG>(S(56))) {
                if (pt.x > static_cast<LONG>(S(700))) return HTCLIENT;
                return HTCAPTION;
            }
            return HTCLIENT;
        }

        case WM_LBUTTONDOWN: {
            if (g_view == ViewMode::Main) return 0;
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};

            // 返回按钮
            if (pt.x >= static_cast<LONG>(S(24)) && pt.x <= static_cast<LONG>(S(56)) &&
                pt.y >= static_cast<LONG>(S(16)) && pt.y <= static_cast<LONG>(S(48))) {
                g_view = ViewMode::Main;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            // 命中滚轮：记录拖动起点，并直接定位到点击处最近的一档
            auto tryHit = [&](Wheel& w, int id) -> bool {
                if (pt.x < static_cast<LONG>(S(w.x)) || pt.x > static_cast<LONG>(S(w.x + w.w))) return false;
                if (pt.y < static_cast<LONG>(S(w.y)) || pt.y > static_cast<LONG>(S(w.y + w.h))) return false;

                g_dragWheel = id;
                g_dragStartY = pt.y;
                g_dragStartSel = w.selected;

                const float cy = S(w.y + w.h * 0.5f);
                int target = w.selected + static_cast<int>(std::lround((pt.y - cy) / S(34.0f)));
                if (target < 0) target = 0;
                if (target >= w.count) target = w.count - 1;
                if (w.selected != target) {
                    w.selected = target;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return true;
            };
            if (g_view == ViewMode::MouseDetail) {
                if (tryHit(g_dpiWheel, 0)) return 0;
                if (tryHit(g_rateWheel, 1)) return 0;
            } else {
                if (tryHit(g_brightWheel, 2)) return 0;
                if (tryHit(g_speedWheel, 3)) return 0;
            }
            return 0;
        }

        case WM_MOUSEMOVE:
            if (g_dragWheel >= 0) {
                POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                Wheel& w = *WheelById(g_dragWheel);
                int target = g_dragStartSel +
                             static_cast<int>(std::lround((g_dragStartY - pt.y) / S(34.0f)));
                if (target < 0) target = 0;
                if (target >= w.count) target = w.count - 1;
                if (w.selected != target) {
                    w.selected = target;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
            return 0;

        case WM_LBUTTONUP: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};

            // 松开滚轮：延迟 400 ms 写入，避免快速滚动时连续写设备
            if (g_dragWheel >= 0) {
                g_pendingWrite = g_dragWheel;
                g_dragWheel = -1;
                SetTimer(hwnd, (g_pendingWrite >= 2) ? 3 : 2, 400, nullptr);
                return 0;
            }

            if (g_view == ViewMode::Main) {
                if (pt.x > static_cast<LONG>(S(700)) && pt.y < static_cast<LONG>(S(56))) {
                    HidePanel();
                    return 0;
                }
                if (pt.y >= static_cast<LONG>(S(64)) && pt.y <= static_cast<LONG>(S(264))) {
                    if (pt.x >= static_cast<LONG>(S(24)) && pt.x <= static_cast<LONG>(S(374))) {
                        SyncWheels();
                        g_view = ViewMode::MouseDetail;
                        InvalidateRect(hwnd, nullptr, FALSE);
                        return 0;
                    }
                    if (pt.x >= static_cast<LONG>(S(386)) && pt.x <= static_cast<LONG>(S(736))) {
                        SyncKbWheels();
                        g_view = ViewMode::KbDetail;
                        InvalidateRect(hwnd, nullptr, FALSE);
                        return 0;
                    }
                }
                return 0;
            }

            if (pt.x >= static_cast<LONG>(S(24)) && pt.x <= static_cast<LONG>(S(56)) &&
                pt.y >= static_cast<LONG>(S(16)) && pt.y <= static_cast<LONG>(S(48))) {
                g_view = ViewMode::Main;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

        case WM_MOUSEWHEEL: {
            if (g_view == ViewMode::Main) return 0;
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &pt);
            const int step = (GET_WHEEL_DELTA_WPARAM(wp) > 0) ? -1 : 1;

            Wheel* w = nullptr;
            int which = -1;
            if (pt.x >= static_cast<LONG>(S(g_dpiWheel.x)) &&
                pt.x <= static_cast<LONG>(S(g_dpiWheel.x + g_dpiWheel.w)) &&
                pt.y >= static_cast<LONG>(S(g_dpiWheel.y)) &&
                pt.y <= static_cast<LONG>(S(g_dpiWheel.y + g_dpiWheel.h))) {
                w = &g_dpiWheel;
                which = 0;
            } else if (pt.x >= static_cast<LONG>(S(g_rateWheel.x)) &&
                       pt.x <= static_cast<LONG>(S(g_rateWheel.x + g_rateWheel.w)) &&
                       pt.y >= static_cast<LONG>(S(g_rateWheel.y)) &&
                       pt.y <= static_cast<LONG>(S(g_rateWheel.y + g_rateWheel.h))) {
                w = &g_rateWheel;
                which = 1;
            } else if (pt.x >= static_cast<LONG>(S(g_brightWheel.x)) &&
                       pt.x <= static_cast<LONG>(S(g_brightWheel.x + g_brightWheel.w)) &&
                       pt.y >= static_cast<LONG>(S(g_brightWheel.y)) &&
                       pt.y <= static_cast<LONG>(S(g_brightWheel.y + g_brightWheel.h))) {
                w = &g_brightWheel;
                which = 2;
            } else if (pt.x >= static_cast<LONG>(S(g_speedWheel.x)) &&
                       pt.x <= static_cast<LONG>(S(g_speedWheel.x + g_speedWheel.w)) &&
                       pt.y >= static_cast<LONG>(S(g_speedWheel.y)) &&
                       pt.y <= static_cast<LONG>(S(g_speedWheel.y + g_speedWheel.h))) {
                w = &g_speedWheel;
                which = 3;
            }
            if (w == nullptr) return 0;

            int target = w->selected + step;
            if (target < 0) target = 0;
            if (target >= w->count) target = w->count - 1;
            if (w->selected != target) {
                w->selected = target;
                g_pendingWrite = which;
                SetTimer(hwnd, (which >= 2) ? 3 : 2, 400, nullptr);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

        case WM_TIMER:
            if (wp == 2) {
                KillTimer(hwnd, 2);
                const int which = g_pendingWrite;
                g_pendingWrite = -1;
                if (which < 0) return 0;

                bool ok = false;
                if (which == 0) {
                    ok = WriteMouseDpi(g_dpiWheel.selected);
                } else {
                    const int usb = g.mouseCfg.ok ? g.mouseCfg.usbRateIndex : g_rateWheel.selected;
                    ok = WriteMouseRate(usb, g_rateWheel.selected);
                }

                if (ok) {
                    if (g.wakeEvent != nullptr) SetEvent(g.wakeEvent);
                } else {
                    SyncWheels();  // 回滚到设备当前值
                    InvalidateRect(hwnd, nullptr, FALSE);
                    MessageBoxW(hwnd, which == 0 ? L"切换 DPI 失败，已回滚。"
                                                : L"切换回报率失败，已回滚。",
                                kAppTitle, MB_OK | MB_ICONWARNING);
                }
            } else if (wp == 3) {
                KillTimer(hwnd, 3);
                g_pendingWrite = -1;
                if (WriteKeyboardLed(kLedModeValues[g_modeWheel.selected], g_brightWheel.selected,
                                     g_speedWheel.selected)) {
                    if (g.wakeEvent != nullptr) SetEvent(g.wakeEvent);
                } else {
                    SyncKbWheels();
                    InvalidateRect(hwnd, nullptr, FALSE);
                    MessageBoxW(hwnd, L"调整灯效失败，已回滚。", kAppTitle, MB_OK | MB_ICONWARNING);
                }
            }
            return 0;

        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) {
                if (g_view != ViewMode::Main) {
                    g_view = ViewMode::Main;
                    InvalidateRect(hwnd, nullptr, FALSE);
                } else {
                    HidePanel();
                }
            }
            return 0;

        case WM_TRAYICON:
            OnTrayMessage(lp);
            return 0;

        case WM_DEVICE_UPDATE: {
            const DeviceReading& r = g.reading;
            g.mouseConnected = r.mouseOk;
            g.mouseBattery = r.mouseBattery;
            g.kbConnected = r.kbOk;
            g.kbBattery = r.kbBattery;

            // 设备是真相源：无拖动、无待写入时，滚轮跟随设备实际档位
            if (g_view == ViewMode::MouseDetail && g_dragWheel < 0 && g_pendingWrite < 0 &&
                g.mouseCfg.ok) {
                const int d = g.mouseCfg.dpiIndex;
                if (d >= 0 && d < g_dpiWheel.count && d != g_dpiWheel.selected) {
                    g_dpiWheel.selected = d;
                }
                const int rt = g.mouseCfg.rateIndex;
                if (rt >= 0 && rt < g_rateWheel.count && rt != g_rateWheel.selected) {
                    g_rateWheel.selected = rt;
                }
            }

            UpdateTrayIcon();
            if (g.panelVisible) InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDM_OPEN:
                    ShowPanel();
                    break;
                case IDM_AUTOSTART:
                    g.autostart = !g.autostart;
                    ApplyAutoStart(g.autostart);
                    SaveConfig();
                    break;
                case IDM_DEBUG:
                    MessageBoxW(hwnd, L"完整调试版在 P5 阶段实现。", kAppTitle, MB_OK | MB_ICONINFORMATION);
                    break;
                case IDM_EXIT:
                    g.exiting = true;
                    DestroyWindow(hwnd);
                    break;
                default:
                    break;
            }
            return 0;

        case WM_POWERBROADCAST:
            // NFR-10：唤醒后重建资源（P2 接入设备时在此重新枚举）
            if (wp == PBT_APMRESUMEAUTOMATIC) {
                if (g.wakeEvent != nullptr) SetEvent(g.wakeEvent);  // 立即重读
            }
            return TRUE;

        case WM_QUERYENDSESSION:
            g.exiting = true;
            return TRUE;

        case WM_ENDSESSION:
            if (wp) {
                RemoveTrayIcon();
                PostQuitMessage(0);
            }
            return 0;

        case WM_CLOSE:
            HidePanel();
            return 0;

        case WM_DESTROY:
            // C7 退出序列：先停工作线程，避免其向已销毁的窗口投递消息
            if (g.stopEvent != nullptr) SetEvent(g.stopEvent);
            if (g.devThread != nullptr) {
                WaitForSingleObject(g.devThread, 3000);
                CloseHandle(g.devThread);
                g.devThread = nullptr;
            }
            if (g.wakeEvent != nullptr) { CloseHandle(g.wakeEvent); g.wakeEvent = nullptr; }
            if (g.stopEvent != nullptr) { CloseHandle(g.stopEvent); g.stopEvent = nullptr; }

            RemoveTrayIcon();
            SaveConfig();
            DiscardDeviceResources();
            if (g.trayIcon != nullptr) { DestroyIcon(g.trayIcon); g.trayIcon = nullptr; }
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }

    // explorer.exe 重启后重挂托盘（NFR-11）
    if (msg == g.taskbarCreated && g.taskbarCreated != 0) {
        AddTrayIcon();
        UpdateTrayIcon();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdLine, int) {
    // 单实例（C7 / FR-T9）
    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (mutex != nullptr && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(kPanelClass, nullptr);
        if (other != nullptr) {
            PostMessageW(other, WM_ACTIVATE_HUB, 0, 0);
            ShowWindow(other, SW_SHOW);
            SetForegroundWindow(other);
        }
        CloseHandle(mutex);
        return 0;
    }

    // Per-Monitor V2（NFR-15），动态获取以兼容旧系统
    using SetDpiCtxFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        auto fn = reinterpret_cast<SetDpiCtxFn>(
            GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        if (fn != nullptr) fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    g.inst = inst;
    ResolveConfigDir();
    LoadConfig();
    FixAutoStartPath();

    bool startHidden = wcsstr(cmdLine, L"--tray") != nullptr;

    // Direct2D / DirectWrite
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g.d2d))) {
        CloseHandle(mutex);
        return 1;
    }
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(&g.dw)))) {
        g.d2d->Release();
        CloseHandle(mutex);
        return 1;
    }
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = PanelProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kPanelClass;
    RegisterClassExW(&wc);

    // 创建前取显示器有效 DPI（PMv2 下 GetDeviceCaps 恒为 96，GetDpiForWindow 在窗口显示前不可靠）
    UINT sysDpi = 96;
    if (HMODULE shcore = LoadLibraryW(L"shcore.dll")) {
        using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
        auto fn = reinterpret_cast<GetDpiForMonitorFn>(GetProcAddress(shcore, "GetDpiForMonitor"));
        if (fn != nullptr) {
            UINT dx = 96;
            UINT dy = 96;
            if (SUCCEEDED(fn(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), 0, &dx, &dy)) &&
                dx > 0) {
                sysDpi = dx;
            }
        }
        FreeLibrary(shcore);
    }
    const int startW = MulDiv(g.winW, static_cast<int>(sysDpi), 96);
    const int startH = MulDiv(g.winH, static_cast<int>(sysDpi), 96);

    int x = g.winX;
    int y = g.winY;
    if (x < 0 || y < 0) {
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi)) {
            x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - startW) / 2;
            y = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - startH) / 2;
        } else {
            x = 100;
            y = 100;
        }
    }

    HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, kPanelClass, kAppTitle,
                                WS_POPUP, x, y, startW, startH,
                                nullptr, nullptr, inst, nullptr);
    if (hwnd == nullptr) {
        g.dw->Release();
        g.d2d->Release();
        CloseHandle(mutex);
        return 1;
    }

    g.dpi = GetDpiForWindow(hwnd);
    if (g.dpi == 0) g.dpi = 96;

    // PMv2 进程下 GetDeviceCaps 恒为 96，必须在窗口创建后用 GetDpiForWindow 换算物理尺寸
    {
        const int pw = MulDiv(g.winW, static_cast<int>(g.dpi), 96);
        const int ph = MulDiv(g.winH, static_cast<int>(g.dpi), 96);
        if (pw != g.winW || ph != g.winH) {
            RECT work{};
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
            if (g.winX < 0 || g.winY < 0) {
                x = work.left + ((work.right - work.left) - pw) / 2;
                y = work.top + ((work.bottom - work.top) - ph) / 2;
            }
            SetWindowPos(hwnd, nullptr, x, y, pw, ph, SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }

    // Win11 圆角
    DWM_WINDOW_CORNER_PREFERENCE pref = DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &pref, sizeof(pref));

    g.taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    AddTrayIcon();

    g.wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);  // 自动重置
    g.stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);   // 手动重置
    if (g.wakeEvent != nullptr && g.stopEvent != nullptr) {
        g.devThread = CreateThread(nullptr, 0, DeviceThreadProc, nullptr, 0, nullptr);
    }

    UpdateTrayIcon();

    if (!startHidden) {
        ShowWindow(hwnd, SW_SHOW);
        g.panelVisible = true;
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_ACTIVATE_HUB) {
            ShowPanel();
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // 显式清理
    if (g.fmtTitle) g.fmtTitle->Release();
    if (g.fmtValue) g.fmtValue->Release();
    if (g.fmtSmall) g.fmtSmall->Release();
    if (g.fmtWheelBig) g.fmtWheelBig->Release();
    if (g.fmtWheelMid) g.fmtWheelMid->Release();
    if (g.fmtWheelSmall) g.fmtWheelSmall->Release();
    if (g.fmtHead) g.fmtHead->Release();
    if (g.dw) g.dw->Release();
    if (g.d2d) g.d2d->Release();
    if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
    CoUninitialize();
    return 0;
}
