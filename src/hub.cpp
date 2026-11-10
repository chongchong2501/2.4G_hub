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

#include <shobjidl.h>
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

// A7 Pro 为 1K 款：回报率仅支持 125 / 500 / 1000
const int kRateValues[6] = {125, 500, 1000, 0, 0, 0};
constexpr int kRateCount = 3;

// 统一逻辑画布：所有 UI 坐标都以 760 × 360 为基准，S() 再按 DPI 映射到物理像素。
// 窗口物理尺寸 = kCanvasW/H × dpi/96，保证"居中"在任何缩放下都是真居中。
constexpr float kCanvasW = 760.0f;
constexpr float kCanvasH = 360.0f;
constexpr float kPadX = 28.0f;

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
    UINT dpi = 96;
    // 绘制缩放由实际客户区尺寸推导，绘制与鼠标命中始终基于同一套坐标。
    float scale = 1.0f;
    float fontScale = 0.0f;

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
    int winW = static_cast<int>(kCanvasW);
    int winH = static_cast<int>(kCanvasH);
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

// 逻辑坐标 → 物理像素。缩放取自渲染目标的实际尺寸，绘制与命中判定共用。
float S(float v) { return v * g.scale; }

// 缩放只认 GetClientRect 这个真实物理尺寸，不用 render target 自报的
// GetSize()（DIP 单位，受 rt DPI 影响），保证绘制与命中判定完全对齐。
void UpdateScale() {
    RECT rc{};
    GetClientRect(g.panel, &rc);
    const float w = static_cast<float>(rc.right - rc.left);
    const float h = static_cast<float>(rc.bottom - rc.top);
    if (w <= 0.0f || h <= 0.0f) return;
    const float sx = w / kCanvasW;
    const float sy = h / kCanvasH;
    g.scale = (sx < sy) ? sx : sy;
}

// ---------- 统一布局：单一坐标源 ----------
//
// 每个界面元素只在这里定义一次，绘制与鼠标命中都读同一份 RectL，
// 杜绝"画在 A 处、点击判定在 B 处"。所有数值均为逻辑坐标。

struct RectL {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
};

struct UiLayout {
    RectL back;          // 详情页返回按钮
    RectL backHit;       // 返回按钮命中区（略放大）
    RectL close;         // 主面板关闭按钮
    RectL closeHit;
    RectL title;         // 顶栏标题
    RectL status;        // 顶栏右侧状态（右对齐）
    RectL foot;          // 底部信息条

    RectL mouseCard;
    RectL kbCard;
    RectL autostart;     // 开机自启勾选框
    RectL autostartHit;

    RectL dpiWheel;
    RectL rateWheel;
    RectL plusBtn;
    RectL minusBtn;

    RectL modeWheel;
    RectL brightWheel;
    RectL speedWheel;
};

UiLayout L;

void InitLayout() {
    // 顶栏：按钮垂直居中，右侧状态贴 kCanvasW - kPadX
    L.back   = {kPadX, 16, 32, 32};
    L.backHit = {kPadX - 4, 12, 40, 40};
    L.close  = {kCanvasW - kPadX - 28, 16, 28, 28};
    L.closeHit = {kCanvasW - kPadX - 36, 8, 44, 44};
    L.title  = {72, 14, 440, 36};
    L.status = {kCanvasW - kPadX - 200, 16, 200, 32};
    L.foot   = {kPadX, 306, kCanvasW - kPadX * 2.0f, 28};

    // 主面板：两张 342 × 200 卡片，间隙 16，整体水平居中 → 左右各留 30
    const float cardW = 342.0f;
    const float cardH = 200.0f;
    const float cardGap = 16.0f;
    const float cardX = (kCanvasW - (cardW * 2.0f + cardGap)) * 0.5f;
    L.mouseCard = {cardX, 72, cardW, cardH};
    L.kbCard    = {cardX + cardW + cardGap, 72, cardW, cardH};
    L.autostart = {cardX + 4, L.foot.y + 5, 18, 18};
    L.autostartHit = {cardX, L.foot.y, 140, L.foot.h};

    // 鼠标详情：两个 248 × 176 滚轮，间隙 56，水平居中 → 左右各留 104
    // 旧布局左右 88、中间 144，两个滚轮被推到两侧、视觉上"不聚拢"
    const float mwW = 248.0f;
    const float mwH = 176.0f;
    const float mwGap = 56.0f;
    const float mwX = (kCanvasW - (mwW * 2.0f + mwGap)) * 0.5f;
    L.dpiWheel  = {mwX, 100, mwW, mwH};
    L.rateWheel = {mwX + mwW + mwGap, 100, mwW, mwH};

    // 增删按钮组在 DPI 滚轮正下方、与其中心线对齐
    const float btnW = 56.0f;
    const float btnH = 28.0f;
    const float btnGap = 12.0f;
    const float btnX = mwX + (mwW - (btnW * 2.0f + btnGap)) * 0.5f;
    L.plusBtn  = {btnX, 292, btnW, btnH};
    L.minusBtn = {btnX + btnW + btnGap, 292, btnW, btnH};

    // 键盘详情：288 + 150 + 150，间隙 20，水平居中 → 左右各留 66
    // 旧布局从 x=24 起、右边缘只到 674，整体比窗口中心偏左 31px
    const float kwGap = 20.0f;
    const float kwTotal = 288.0f + kwGap + 150.0f + kwGap + 150.0f;
    const float kwX = (kCanvasW - kwTotal) * 0.5f;
    L.modeWheel   = {kwX, 100, 288, 186};
    L.brightWheel = {kwX + 288.0f + kwGap, 100, 150, 186};
    L.speedWheel  = {kwX + 288.0f + kwGap + 150.0f + kwGap, 100, 150, 186};
}

// 逻辑矩形 → 物理像素矩形（render target 固定 96 DPI，1 单位 = 1 物理像素）
D2D1_RECT_F R(const RectL& r) {
    return D2D1::RectF(S(r.x), S(r.y), S(r.x + r.w), S(r.y + r.h));
}
D2D1_RECT_F R(float x, float y, float w, float h) {
    return D2D1::RectF(S(x), S(y), S(x + w), S(y + h));
}

// 客户区物理坐标是否落在逻辑矩形内
bool Hit(POINT pt, const RectL& r) {
    return pt.x >= static_cast<LONG>(S(r.x)) && pt.x <= static_cast<LONG>(S(r.x + r.w)) &&
           pt.y >= static_cast<LONG>(S(r.y)) && pt.y <= static_cast<LONG>(S(r.y + r.h));
}

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

// 双环托盘图标：外环 = 键盘电量，内环 = 鼠标电量
HICON MakeTrayIcon(int mouseLevel, int kbLevel) {
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

    auto put = [&](int x, int y, uint8_t r, uint8_t gr, uint8_t b, uint8_t a) {
        if (x < 0 || y < 0 || x >= sz || y >= sz) return;
        px[y * sz + x] = (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(r) << 16) |
                         (static_cast<uint32_t>(gr) << 8) | b;
    };

    // 电量 → 颜色
    auto colorFor = [](int level, uint8_t* r, uint8_t* g2, uint8_t* b) {
        if (level < 0) { *r = 0x55; *g2 = 0x57; *b = 0x5B; return; }        // 离线：灰
        if (level < 20) { *r = 0xE2; *g2 = 0x4B; *b = 0x4A; return; }       // 低：红
        if (level < 50) { *r = 0xEF; *g2 = 0x9F; *b = 0x27; return; }       // 中：黄
        *r = 0x5C; *g2 = 0xA8; *b = 0x34;                                    // 高：绿
    };

    uint8_t mr, mg, mb, kr, kg, kb;
    colorFor(mouseLevel, &mr, &mg, &mb);
    colorFor(kbLevel, &kr, &kg, &kb);

    const int mousePct = (mouseLevel < 0) ? 0 : (mouseLevel > 100 ? 100 : mouseLevel);
    const int kbPct = (kbLevel < 0) ? 0 : (kbLevel > 100 ? 100 : kbLevel);

    const float cx = 15.5f, cy = 15.5f;
    const float kTwoPi = 6.28318531f;
    const float rOut = 13.5f, thOut = 3.6f;   // 外环（键盘）
    const float rIn = 7.5f, thIn = 3.6f;      // 内环（鼠标）
    const uint8_t dimR = 0x3F, dimG = 0x41, dimB = 0x45;

    // 单个采样点：返回该点的颜色与可见性
    auto sampleAt = [&](float fx, float fy, int* rr, int* gg, int* bb, int* aa) {
        const float dx = fx - cx;
        const float dy = fy - cy;
        const float dist = std::sqrt(dx * dx + dy * dy);

        float ang = std::atan2(dx, -dy);
        if (ang < 0.0f) ang += kTwoPi;
        const float frac = ang / kTwoPi;

        const bool inOut = std::fabs(dist - rOut) <= thOut * 0.5f;
        const bool inIn = std::fabs(dist - rIn) <= thIn * 0.5f;

        if (inOut) {
            if (frac <= kbPct / 100.0f) { *rr = kr; *gg = kg; *bb = kb; }
            else { *rr = dimR; *gg = dimG; *bb = dimB; }
            *aa = 255;
            return;
        }
        if (inIn) {
            if (frac <= mousePct / 100.0f) { *rr = mr; *gg = mg; *bb = mb; }
            else { *rr = dimR; *gg = dimG; *bb = dimB; }
            *aa = 255;
            return;
        }
        *rr = 0; *gg = 0; *bb = 0; *aa = 0;
    };

    // 4×4 超采样：先按 4 倍精度判断，再平均回单像素
    const int kSS = 4;
    for (int y = 0; y < sz; ++y) {
        for (int x = 0; x < sz; ++x) {
            int sumR = 0, sumG = 0, sumB = 0, sumA = 0;
            for (int syy = 0; syy < kSS; ++syy) {
                for (int sxx = 0; sxx < kSS; ++sxx) {
                    const float fx = static_cast<float>(x) + (sxx + 0.5f) / kSS;
                    const float fy = static_cast<float>(y) + (syy + 0.5f) / kSS;
                    int rr, gg, bb, aa;
                    sampleAt(fx, fy, &rr, &gg, &bb, &aa);
                    // 先乘 alpha 再累加，避免边缘出现色边
                    sumR += rr * aa / 255;
                    sumG += gg * aa / 255;
                    sumB += bb * aa / 255;
                    sumA += aa;
                }
            }
            const int n = kSS * kSS;
            const int a = sumA / n;
            if (a == 0) continue;
            put(x, y, static_cast<uint8_t>(sumR / n), static_cast<uint8_t>(sumG / n),
                static_cast<uint8_t>(sumB / n), static_cast<uint8_t>(a));
        }
    }

    SelectObject(dc, old);
    DeleteDC(dc);

    HBITMAP mask = CreateBitmap(sz, sz, 1, 1, nullptr);
    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmColor = color;
    ii.hbmMask = mask;
    HICON icon = CreateIconIndirect(&ii);

    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}

// ---------- 任务栏（缩略图角标 + 悬停提示） ----------

ITaskbarList3* g_taskbar = nullptr;

bool InitTaskbar() {
    if (g_taskbar != nullptr) return true;
    if (FAILED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&g_taskbar)))) {
        g_taskbar = nullptr;
        return false;
    }
    if (FAILED(g_taskbar->HrInit())) {
        g_taskbar->Release();
        g_taskbar = nullptr;
        return false;
    }
    return true;
}

// 16×16 实心圆角标，颜色按电量分档
HICON MakeBadgeIcon(int level) {
    const int size = 16;
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = size;
    bi.bV5Height = -size;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;

    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bmp = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS,
                                   &bits, nullptr, 0);
    if (bmp == nullptr || bits == nullptr) {
        if (bmp) DeleteObject(bmp);
        DeleteDC(dc);
        return nullptr;
    }
    SelectObject(dc, bmp);

    DWORD color = 0x00639922;                      // 绿
    if (level < 20) color = 0x004A4BE2;            // 红（BGR）
    else if (level < 50) color = 0x00279FEF;       // 琥珀（BGR）

    auto* px = static_cast<uint32_t*>(bits);
    const float c = (size - 1) / 2.0f;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const float dx = x - c;
            const float dy = y - c;
            const float dist = std::sqrt(dx * dx + dy * dy);
            // 边缘 1px 做线性渐变，避免 16×16 下出现八边形锯齿
            float alpha = c - dist + 0.5f;
            if (alpha <= 0.0f) { px[y * size + x] = 0u; continue; }
            if (alpha > 1.0f) alpha = 1.0f;
            px[y * size + x] = (static_cast<uint32_t>(alpha * 255.0f) << 24) | color;
        }
    }

    SelectObject(dc, GetStockObject(NULL_BRUSH));
    DeleteDC(dc);

    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmColor = bmp;
    ii.hbmMask = mask;
    HICON icon = CreateIconIndirect(&ii);

    DeleteObject(bmp);
    DeleteObject(mask);
    return icon;
}

void UpdateTaskbar() {
    if (g.panel == nullptr || !InitTaskbar()) return;

    const int mouseLevel = g.mouseConnected ? g.mouseBattery : -1;
    const int kbLevel = g.kbConnected ? g.kbBattery : -1;
    int worst = -1;
    if (mouseLevel >= 0 && kbLevel >= 0) worst = (mouseLevel < kbLevel) ? mouseLevel : kbLevel;
    else if (mouseLevel >= 0) worst = mouseLevel;
    else if (kbLevel >= 0) worst = kbLevel;

    wchar_t tip[160];
    if (worst < 0) {
        wcscpy_s(tip, L"MCHOSE Hub · 设备离线");
    } else {
        swprintf_s(tip, L"鼠标 %d%%　·　键盘 %d%%", g.mouseBattery, g.kbBattery);
    }
    g_taskbar->SetThumbnailTooltip(g.panel, tip);

    HICON badge = (worst >= 0) ? MakeBadgeIcon(worst) : nullptr;
    g_taskbar->SetOverlayIcon(g.panel, badge, tip);
    if (badge != nullptr) DestroyIcon(badge);
}

void UpdateTrayIcon() {
    HICON old = g.trayIcon;
    const int mouseLevel = g.mouseConnected ? g.mouseBattery : -1;
    const int kbLevel = g.kbConnected ? g.kbBattery : -1;
    g.trayIcon = MakeTrayIcon(mouseLevel, kbLevel);

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
    if (g.trayIcon == nullptr) g.trayIcon = MakeTrayIcon(-1, -1);
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
        const float px = s.size * g.scale;
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
    RECT rc{};
    GetClientRect(g.panel, &rc);
    const D2D1_SIZE_U size = D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top);
    if (size.width == 0 || size.height == 0) return false;
    if (g.rt != nullptr) {
        // 窗口尺寸可能被 DPI 通知改动，这里按客户区同步一次（相同则跳过）
        const D2D1_SIZE_U cur = g.rt->GetPixelSize();
        if (cur.width != size.width || cur.height != size.height) g.rt->Resize(size);
        return true;
    }
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

// 文字在矩形内按指定方式对齐。临时改 format 属性、画完还原，
// 不污染其他复用同一 format 的绘制点（滚轮格式本身已居中）。
void DrawTextAligned(const wchar_t* text, IDWriteTextFormat* fmt, const D2D1_RECT_F& rc,
                     const D2D1_COLOR_F& color,
                     DWRITE_TEXT_ALIGNMENT h = DWRITE_TEXT_ALIGNMENT_LEADING,
                     DWRITE_PARAGRAPH_ALIGNMENT v = DWRITE_PARAGRAPH_ALIGNMENT_CENTER) {
    if (fmt == nullptr) return;
    const DWRITE_TEXT_ALIGNMENT oldH = fmt->GetTextAlignment();
    const DWRITE_PARAGRAPH_ALIGNMENT oldV = fmt->GetParagraphAlignment();
    fmt->SetTextAlignment(h);
    fmt->SetParagraphAlignment(v);
    DrawTextAt(text, fmt, rc, color);
    fmt->SetTextAlignment(oldH);
    fmt->SetParagraphAlignment(oldV);
}

constexpr DWRITE_TEXT_ALIGNMENT kLeft = DWRITE_TEXT_ALIGNMENT_LEADING;
constexpr DWRITE_TEXT_ALIGNMENT kCenter = DWRITE_TEXT_ALIGNMENT_CENTER;
constexpr DWRITE_TEXT_ALIGNMENT kRight = DWRITE_TEXT_ALIGNMENT_TRAILING;
constexpr DWRITE_PARAGRAPH_ALIGNMENT kMiddle = DWRITE_PARAGRAPH_ALIGNMENT_CENTER;

void FillRound(const D2D1_RECT_F& rc, float radius, const D2D1_COLOR_F& color) {
    ID2D1SolidColorBrush* brush = nullptr;
    if (SUCCEEDED(g.rt->CreateSolidColorBrush(color, &brush))) {
        g.rt->FillRoundedRectangle(
            D2D1::RoundedRect(rc, S(radius), S(radius)), brush);
        brush->Release();
    }
}

void DrawCard(const RectL& c, const wchar_t* name, const wchar_t* model,
              const wchar_t* link, const wchar_t* valueOverride, int battery, bool connected) {
    FillRound(R(c), 12, kCard);

    const float pad = 20.0f;
    const float innerW = c.w - pad * 2.0f;

    DrawTextAligned(name, g.fmtTitle, R(c.x + pad, c.y + 18, innerW, 26), kText,
                    kLeft, kMiddle);
    DrawTextAligned(link, g.fmtSmall, R(c.x + pad, c.y + 46, innerW, 20), kTextDim,
                    kLeft, kMiddle);

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
    DrawTextAligned(val, g.fmtValue, R(c.x + pad, c.y + 74, innerW, 44),
                    connected ? kText : kTextDim, kLeft, kMiddle);
    DrawTextAligned(model, g.fmtSmall, R(c.x + pad, c.y + 122, innerW, 20), kTextDim,
                    kLeft, kMiddle);

    // 电量条贴卡片底部，左右对称留 pad；半径按条高的一半
    const float barY = c.y + c.h - 34.0f;
    FillRound(R(c.x + pad, barY, innerW, 6), 3, kLine);
    if (connected && battery > 0) {
        const float pct = battery > 100 ? 1.0f : battery / 100.0f;
        const float fillW = innerW * pct;
        FillRound(R(c.x + pad, barY, fillW, 6), std::fmin(3.0f, fillW * 0.5f),
                  battery < 20 ? kBad : (battery < 50 ? kWarn : kAccent));
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
const int kLedModeValues[21] = {9999, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
                                11, 12, 13, 14, 15, 16, 17, 18, 19, 254};
const wchar_t* kLedModeNames[21] = {
    L"关闭灯效", L"静态恒亮", L"按压点亮", L"释放熄灭", L"繁星点点", L"漫天飞雪",
    L"百花争艳", L"动态呼吸", L"光谱循环", L"彩泉涌动", L"多彩纵横", L"随波逐流",
    L"峰回路转", L"一触即发", L"一石二鸟", L"涟漪扩散", L"川流不息", L"重峦叠嶂",
    L"斜风细雨", L"来回穿梭", L"心动时刻"};
constexpr int kLedModeCount = 21;

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
bool g_dpiInputMode = false;      // 正在输入新 DPI
int g_dpiInsertPos = -1;          // 新档插入位置（当前档的下一个）
std::wstring g_dpiInputText;
bool g_cursorVisible = true;

D2D1_COLOR_F AlphaText(float alpha) {
    return D2D1::ColorF(0.902f, 0.910f, 0.922f, alpha);
}

// 命中判定与绘制共用同一份滚轮矩形
bool HitWheel(POINT pt, const Wheel& wh) {
    return Hit(pt, RectL{wh.x, wh.y, wh.w, wh.h});
}

// 时钟式滚轮：中间选中项最大最亮，上下各两行逐级缩小变淡
void DrawWheel(const Wheel& wh, const std::vector<std::wstring>& labels, const wchar_t* title) {
    FillRound(R(wh.x, wh.y, wh.w, wh.h), 14, kCard);

    // 标题居中在卡片正上方
    DrawTextAligned(title, g.fmtSmall, R(wh.x, wh.y - 30, wh.w, 24), kTextDim,
                    kCenter, kMiddle);

    const float cy = wh.y + wh.h * 0.5f;
    const float itemH = 34.0f;

    // 选中行的底衬，左右对称
    FillRound(R(wh.x + 12, cy - itemH * 0.5f, wh.w - 24, itemH), 9,
              D2D1::ColorF(0.155f, 0.165f, 0.185f));

    for (int d = -2; d <= 2; ++d) {
        const int idx = wh.selected + d;
        if (idx < 0 || idx >= wh.count || idx >= static_cast<int>(labels.size())) continue;

        const float y = cy + d * itemH;
        const float alpha = (d == 0) ? 1.0f : ((d == 1 || d == -1) ? 0.42f : 0.16f);
        IDWriteTextFormat* fmt = (d == 0) ? g.fmtWheelBig
                                          : ((d == 1 || d == -1) ? g.fmtWheelMid : g.fmtWheelSmall);
        DrawTextAligned(labels[idx].c_str(), fmt,
                        R(wh.x, y - itemH * 0.5f, wh.w, itemH),
                        AlphaText(alpha), kCenter, kMiddle);
    }
}

void DrawBackButton() {
    FillRound(R(L.back), 8, kCard);
    DrawTextAligned(L"←", g.fmtHead, R(L.back), kText, kCenter, kMiddle);
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

void PlaceWheel(Wheel& wh, const RectL& r) {
    wh.x = r.x;
    wh.y = r.y;
    wh.w = r.w;
    wh.h = r.h;
}

void SyncWheels() {
    PlaceWheel(g_dpiWheel, L.dpiWheel);
    g_dpiWheel.count = (g.mouseCfg.ok && g.mouseCfg.dpiCount > 0) ? g.mouseCfg.dpiCount : 6;
    g_dpiWheel.selected = (g.mouseCfg.ok && g.mouseCfg.dpiIndex >= 0) ? g.mouseCfg.dpiIndex : 0;
    if (g_dpiWheel.selected >= g_dpiWheel.count) g_dpiWheel.selected = g_dpiWheel.count - 1;

    PlaceWheel(g_rateWheel, L.rateWheel);
    g_rateWheel.count = kRateCount;
    g_rateWheel.selected = (g.mouseCfg.ok && g.mouseCfg.rateIndex >= 0) ? g.mouseCfg.rateIndex : 0;
    if (g_rateWheel.selected >= kRateCount) g_rateWheel.selected = kRateCount - 1;
}

// ---------- 三个视图 ----------

void OnPaintMain() {
    DrawTextAligned(L"设备中心", g.fmtHead, R(L.title), kText, kLeft, kMiddle);
    DrawTextAligned(L"×", g.fmtHead, R(L.close), kTextDim, kCenter, kMiddle);

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
    DrawCard(L.mouseCard, L"鼠标", mouseCfgText,
             g.mouseConnected ? L"2.4G · 已连接" : (mouseSleep ? L"移动鼠标唤醒" : L"未插入接收器"),
             g.mouseConnected ? nullptr : (mouseSleep ? L"无响应" : L"未连接"),
             g.mouseBattery, g.mouseConnected);

    const bool kbSleep = !g.kbConnected && g.reading.kbPresent;
    DrawCard(L.kbCard, L"键盘",
             g.kbConnected ? L"EWEADN X87" : (kbSleep ? L"敲击任意键唤醒" : L"请确认已开机"),
             g.kbConnected ? L"2.4G · 已连接" : (kbSleep ? L"休眠中" : L"未检测到设备"),
             g.kbConnected ? nullptr : (kbSleep ? L"休眠" : L"未连接"),
             g.kbBattery, g.kbConnected);

    // 底部条：勾选框靠左、提示居中，两者基线一致
    FillRound(R(L.autostart), 4,
              g.autostart ? D2D1::ColorF(0.36f, 0.66f, 0.20f) : kCard);
    if (g.autostart) {
        DrawTextAligned(L"✓", g.fmtSmall, R(L.autostart), kText, kCenter, kMiddle);
    }
    DrawTextAligned(L"开机自启", g.fmtSmall,
                    R(L.autostart.x + L.autostart.w + 8, L.foot.y, 90, L.foot.h),
                    kTextDim, kLeft, kMiddle);
    DrawTextAligned(L"点击设备卡片进入设置　·　Esc 关闭", g.fmtSmall,
                    R(L.foot), kTextDim, kCenter, kMiddle);
}

void OnPaintMouseDetail() {
    DrawBackButton();
    DrawTextAligned(L"鼠标 · MCHOSE A7 Pro", g.fmtHead, R(L.title), kText, kLeft, kMiddle);

    wchar_t status[64];
    if (g.mouseConnected) {
        swprintf_s(status, L"%d%%　2.4G", g.mouseBattery);
    } else {
        wcscpy_s(status, L"离线");
    }
    DrawTextAligned(status, g.fmtSmall, R(L.status), kTextDim, kRight, kMiddle);

    std::vector<std::wstring> dpiLabels;
    std::vector<std::wstring> rateLabels;
    const int stageCount = g.mouseCfg.ok ? g.mouseCfg.dpiCount : 0;
    std::wstring inputLine;
    if (g_dpiInputMode) {
        inputLine = g_dpiInputText.empty() ? std::wstring(L"输入 DPI") : g_dpiInputText;
        if (g_dpiInputText.empty() || g_cursorVisible) inputLine += L"|";
    }
    for (int i = 0; i < stageCount; ++i) {
        if (g_dpiInputMode && i == g_dpiInsertPos) dpiLabels.emplace_back(inputLine);
        wchar_t b[24];
        swprintf_s(b, L"%d", g.mouseCfg.dpiList[i]);
        dpiLabels.emplace_back(b);
    }
    if (g_dpiInputMode && g_dpiInsertPos >= stageCount) dpiLabels.emplace_back(inputLine);
    for (int i = 0; i < kRateCount; ++i) {
        wchar_t b[24];
        swprintf_s(b, L"%d", kRateValues[i]);
        rateLabels.emplace_back(b);
    }

    DrawWheel(g_dpiWheel, dpiLabels, L"DPI");
    DrawWheel(g_rateWheel, rateLabels, L"回报率（Hz）");

    // DPI 档位增删按钮：紧贴 DPI 滚轮下方并与其中心线对齐
    FillRound(R(L.plusBtn), 8, D2D1::ColorF(0.175f, 0.185f, 0.205f));
    FillRound(R(L.minusBtn), 8, D2D1::ColorF(0.175f, 0.185f, 0.205f));
    DrawTextAligned(L"+", g.fmtWheelMid, R(L.plusBtn), kText, kCenter, kMiddle);
    DrawTextAligned(L"−", g.fmtWheelMid, R(L.minusBtn), kText, kCenter, kMiddle);

    // 提示右对齐到右边距，左边界让开 +/− 按钮组，避免长文案被 CLIP 裁掉开头
    const float hintX = L.minusBtn.x + L.minusBtn.w + 20.0f;
    DrawTextAligned(L"滚轮选择　·　+ / − 增删档位　·　松开后自动应用", g.fmtSmall,
                    R(hintX, L.plusBtn.y, kCanvasW - kPadX - hintX, L.plusBtn.h),
                    kTextDim, kRight, kMiddle);
}

void SyncKbWheels() {
    PlaceWheel(g_modeWheel, L.modeWheel);
    g_modeWheel.count = kLedModeCount;
    g_modeWheel.selected = LedModeIndex(g.kbLed.ok ? g.kbLed.mode : 1);

    PlaceWheel(g_brightWheel, L.brightWheel);
    g_brightWheel.count = 6;
    g_brightWheel.selected = g.kbLed.ok ? g.kbLed.brightness : 0;
    if (g_brightWheel.selected < 0) g_brightWheel.selected = 0;
    if (g_brightWheel.selected > 5) g_brightWheel.selected = 5;

    PlaceWheel(g_speedWheel, L.speedWheel);
    g_speedWheel.count = 6;
    g_speedWheel.selected = g.kbLed.ok ? g.kbLed.speed : 0;
    if (g_speedWheel.selected < 0) g_speedWheel.selected = 0;
    if (g_speedWheel.selected > 5) g_speedWheel.selected = 5;
}

void OnPaintKbDetail() {
    DrawBackButton();
    DrawTextAligned(L"键盘 · EWEADN X87", g.fmtHead, R(L.title), kText, kLeft, kMiddle);

    wchar_t status[64];
    if (g.kbConnected) {
        swprintf_s(status, L"%d%%　2.4G", g.kbBattery);
    } else {
        wcscpy_s(status, L"离线");
    }
    DrawTextAligned(status, g.fmtSmall, R(L.status), kTextDim, kRight, kMiddle);

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

    DrawTextAligned(L"上下滚动选择　·　松开后自动应用", g.fmtSmall,
                    R(L.foot), kTextDim, kCenter, kMiddle);
}

void OnPaint() {
    if (!CreateDeviceResources()) return;
    UpdateScale();
    // 缩放变化时字号必须同步重建，否则文字大小与卡片/滚轮脱节
    if (g.fmtHead == nullptr || std::fabs(g.fontScale - g.scale) > 0.001f) {
        CreateTextFormats();
        g.fontScale = g.scale;
    }

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
    // 真正隐藏：任务栏不留窗口按钮，电量由托盘图标表达
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
            InitLayout();
            SyncWheels();
            SyncKbWheels();
            return 0;

        case WM_PAINT: {
            PAINTSTRUCT ps{};
            BeginPaint(hwnd, &ps);

            // DPI 变化时字号与 render target 需要重建；缩放本身由 OnPaint
            // 按实际客户区尺寸推导，与 DPI 感知模式无关。
            const UINT dpi = GetDpiForWindow(hwnd);
            if (dpi > 0 && dpi != g.dpi) {
                g.dpi = dpi;
                g.fontScale = 0.0f;
                DiscardDeviceResources();
            }

            OnPaint();
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_SIZE:
            if (wp == SIZE_MINIMIZED) {
                g.panelVisible = false;
                return 0;
            }
            g.panelVisible = (wp != SIZE_MINIMIZED);
            if (g.rt != nullptr && wp != SIZE_MINIMIZED) {
                g.rt->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
            }
            return 0;

        case WM_DPICHANGED: {
            g.dpi = HIWORD(wp);
            if (g.dpi == 0) g.dpi = 96;
            g.fontScale = 0.0f;  // 下一帧按新 scale 重建字号
            DiscardDeviceResources();

            // 系统建议矩形只取左上角位置，宽高按逻辑画布 × 新 DPI 重算，
            // 保证换到不同缩放的显示器后窗口大小一致。
            const RECT* prc = reinterpret_cast<const RECT*>(lp);
            const int wantW = static_cast<int>(std::lround(kCanvasW * g.dpi / 96.0f));
            const int wantH = static_cast<int>(std::lround(kCanvasH * g.dpi / 96.0f));
            SetWindowPos(hwnd, nullptr, prc->left, prc->top, wantW, wantH,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_NCHITTEST: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &pt);
            // 顶部 56px 可拖动；关闭按钮与详情页返回按钮除外
            if (pt.y < static_cast<LONG>(S(56))) {
                if (g_view == ViewMode::Main && Hit(pt, L.closeHit)) return HTCLIENT;
                if (g_view != ViewMode::Main && Hit(pt, L.backHit)) return HTCLIENT;
                return HTCAPTION;
            }
            return HTCLIENT;
        }

        case WM_LBUTTONDOWN: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (g_view == ViewMode::Main) return 0;

            // 返回按钮
            if (Hit(pt, L.backHit)) {
                g_view = ViewMode::Main;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            // 命中滚轮：记录拖动起点，并直接定位到点击处最近的一档
            auto tryHit = [&](Wheel& w, int id) -> bool {
                if (!HitWheel(pt, w)) return false;

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
                // 增删 DPI 档位：命中区与绘制矩形完全一致
                const bool inPlus = Hit(pt, L.plusBtn);
                const bool inMinus = Hit(pt, L.minusBtn);
                if (inPlus || inMinus) {
                    if (!g.mouseCfg.ok) return 0;
                    int count = g.mouseCfg.dpiCount;
                    if (count < 1) count = 1;
                    int vals[6] = {};
                    for (int i = 0; i < count && i < 6; ++i) vals[i] = g.mouseCfg.dpiList[i];

                    if (inPlus) {
                        if (count >= 6) return 0;
                        // 新档插到当前档的下一个位置，等待用户填数值
                        g_dpiInputMode = true;
                        g_dpiInputText.clear();
                        g_cursorVisible = true;
                        g_dpiInsertPos = g_dpiWheel.selected + 1;
                        if (g_dpiInsertPos > count) g_dpiInsertPos = count;
                        g_dpiWheel.count = count + 1;
                        g_dpiWheel.selected = g_dpiInsertPos;
                        SetTimer(hwnd, 4, 530, nullptr);
                        InvalidateRect(hwnd, nullptr, FALSE);
                        return 0;
                    } else {
                        // 删掉当前档，活动档退回上一档
                        if (count <= 1) return 0;
                        int del = g_dpiWheel.selected;
                        if (del < 0 || del >= count) del = count - 1;
                        int kept[6];
                        int n = 0;
                        for (int i = 0; i < count; ++i) {
                            if (i != del) kept[n++] = g.mouseCfg.dpiList[i];
                        }
                        int active = (del > 0) ? (del - 1) : 0;
                        if (active >= n) active = n - 1;
                        SetMouseDpiStages(kept, n, active);
                    }
                    if (g.wakeEvent != nullptr) SetEvent(g.wakeEvent);
                    return 0;
                }

                if (tryHit(g_dpiWheel, 0)) return 0;
                if (tryHit(g_rateWheel, 1)) return 0;
            } else {
                if (tryHit(g_modeWheel, 2)) return 0;
                if (tryHit(g_brightWheel, 3)) return 0;
                if (tryHit(g_speedWheel, 4)) return 0;
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
                if (Hit(pt, L.closeHit)) {
                    HidePanel();
                    return 0;
                }
                // 开机自启勾选
                if (Hit(pt, L.autostartHit)) {
                    g.autostart = !g.autostart;
                    ApplyAutoStart(g.autostart);
                    SaveConfig();
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
                if (Hit(pt, L.mouseCard)) {
                    SyncWheels();
                    g_view = ViewMode::MouseDetail;
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
                if (Hit(pt, L.kbCard)) {
                    SyncKbWheels();
                    g_view = ViewMode::KbDetail;
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
                return 0;
            }

            if (Hit(pt, L.backHit)) {
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
            const bool mousePage = (g_view == ViewMode::MouseDetail);
            const bool kbPage = (g_view == ViewMode::KbDetail);

            if (mousePage && HitWheel(pt, g_dpiWheel)) {
                w = &g_dpiWheel; which = 0;
            } else if (mousePage && HitWheel(pt, g_rateWheel)) {
                w = &g_rateWheel; which = 1;
            } else if (kbPage && HitWheel(pt, g_modeWheel)) {
                w = &g_modeWheel; which = 2;
            } else if (kbPage && HitWheel(pt, g_brightWheel)) {
                w = &g_brightWheel; which = 3;
            } else if (kbPage && HitWheel(pt, g_speedWheel)) {
                w = &g_speedWheel; which = 4;
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
            } else if (wp == 4) {
                g_cursorVisible = !g_cursorVisible;
                InvalidateRect(hwnd, nullptr, FALSE);
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

        case WM_CHAR:
            if (g_dpiInputMode) {
                if (wp >= L'0' && wp <= L'9' && g_dpiInputText.size() < 5) {
                    g_dpiInputText.push_back(static_cast<wchar_t>(wp));
                    InvalidateRect(hwnd, nullptr, FALSE);
                } else if (wp == VK_BACK && !g_dpiInputText.empty()) {
                    g_dpiInputText.pop_back();
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }
            break;

        case WM_KEYDOWN:
            if (g_dpiInputMode) {
                if (wp == VK_RETURN && !g_dpiInputText.empty()) {
                    // 先落状态再写设备：写操作耗时数百 ms，期间若重入会重复插入
                    const std::wstring typed = g_dpiInputText;
                    const int pos = (g_dpiInsertPos >= 0) ? g_dpiInsertPos : -1;
                    g_dpiInputMode = false;
                    g_dpiInputText.clear();
                    g_dpiInsertPos = -1;
                    KillTimer(hwnd, 4);

                    int v = static_cast<int>(wcstol(typed.c_str(), nullptr, 10));
                    if (v < 100) v = 100;
                    if (v > 26000) v = 26000;
                    v = (v / 50) * 50;

                    int count = g.mouseCfg.ok ? g.mouseCfg.dpiCount : 0;
                    if (count >= 1 && count < 6) {
                        int insertAt = (pos >= 0 && pos <= count) ? pos : count;
                        int vals[7];
                        int n = 0;
                        for (int i = 0; i < count; ++i) {
                            if (i == insertAt) vals[n++] = v;
                            vals[n++] = g.mouseCfg.dpiList[i];
                        }
                        if (insertAt >= count) vals[n++] = v;

                        // 从小到大排序
                        for (int i = 0; i < n - 1; ++i) {
                            for (int j = i + 1; j < n; ++j) {
                                if (vals[j] < vals[i]) {
                                    const int t = vals[i];
                                    vals[i] = vals[j];
                                    vals[j] = t;
                                }
                            }
                        }
                        // 活动档指向刚输入的值
                        int active = 0;
                        for (int i = 0; i < n; ++i) {
                            if (vals[i] == v) { active = i; break; }
                        }
                        SetMouseDpiStages(vals, n, active);
                        if (g.wakeEvent != nullptr) SetEvent(g.wakeEvent);
                    }
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
                if (wp == VK_ESCAPE) {
                    g_dpiInputMode = false;
                    g_dpiInputText.clear();
                    g_dpiInsertPos = -1;
                    KillTimer(hwnd, 4);
                    if (g.mouseCfg.ok) {
                        g_dpiWheel.selected = (g.mouseCfg.dpiIndex >= 0) ? g.mouseCfg.dpiIndex : 0;
                    }
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
                return 0;
            }
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
                g.mouseCfg.ok && !g_dpiInputMode) {
                const int d = g.mouseCfg.dpiIndex;
                if (d >= 0 && d < g_dpiWheel.count && d != g_dpiWheel.selected) {
                    g_dpiWheel.selected = d;
                }
                if (g.mouseCfg.dpiCount > 0 && g.mouseCfg.dpiCount != g_dpiWheel.count) {
                    g_dpiWheel.count = g.mouseCfg.dpiCount;
                }
                const int rt = g.mouseCfg.rateIndex;
                if (rt >= 0 && rt < g_rateWheel.count && rt != g_rateWheel.selected) {
                    g_rateWheel.selected = rt;
                }
            }

            UpdateTrayIcon();
            UpdateTaskbar();
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
    g.scale = static_cast<float>(g.dpi) / 96.0f;
    g.fontScale = 0.0f;

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

    UpdateTaskbar();  // 建立任务栏角标

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
