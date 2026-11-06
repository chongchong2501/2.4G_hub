// mouse_ratetest.cpp - 一次性判定 nibble 归属（可逆，测试后立即恢复）
//
// 方法：写 0x11 0x41 (usbRate, freeRate=2)，读回整包，看哪个 nibble 从原值变成 2。
//       随后写回原值，并再次读回确认恢复。
//
// 风险控制：只改回报率索引，不动 DPI / 按键 / 灯效；全程 < 1 秒；结束时恢复原值。

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>

#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")

namespace {

constexpr USHORT kMouseVid = 0x5253;
constexpr USAGE kConfigPage = 0xFF01;

std::wstring FindIface(USHORT vid, USAGE page) {
    GUID g{};
    HidD_GetHidGuid(&g);
    HDEVINFO devs = SetupDiGetClassDevsW(&g, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devs == INVALID_HANDLE_VALUE) return std::wstring();
    std::wstring found;
    for (DWORD i = 0; found.empty(); ++i) {
        SP_DEVICE_INTERFACE_DATA itf = {sizeof(itf)};
        if (!SetupDiEnumDeviceInterfaces(devs, nullptr, &g, i, &itf)) break;
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(devs, &itf, nullptr, 0, &need, nullptr);
        if (need == 0) continue;
        std::vector<BYTE> raw(need);
        auto* d = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(raw.data());
        d->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(devs, &itf, d, need, nullptr, nullptr)) continue;
        HANDLE h = CreateFileW(d->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES a{};
        a.Size = sizeof(a);
        if (HidD_GetAttributes(h, &a) && a.VendorID == vid) {
            PHIDP_PREPARSED_DATA prep = nullptr;
            if (HidD_GetPreparsedData(h, &prep) && prep) {
                HIDP_CAPS c{};
                if (HidP_GetCaps(prep, &c) == HIDP_STATUS_SUCCESS && c.UsagePage == page) {
                    found = d->DevicePath;
                }
                HidD_FreePreparsedData(prep);
            }
        }
        CloseHandle(h);
    }
    SetupDiDestroyDeviceInfoList(devs);
    return found;
}

bool ReadConfig(HANDLE h, std::vector<BYTE>* payload) {
    BYTE frame[65];
    memset(frame, 0, sizeof(frame));
    frame[0] = 0x12;
    frame[1] = 0x67;
    for (int i = 1; i < 65; ++i) frame[i] = static_cast<BYTE>(frame[i] ^ 0xFF);
    if (!HidD_SetFeature(h, frame, 65)) return false;
    Sleep(90);
    BYTE resp[65] = {0x12};
    if (!HidD_GetFeature(h, resp, 65)) return false;
    if (static_cast<BYTE>(resp[1] ^ 0xFF) != 0x67) return false;
    payload->assign(resp + 2, resp + 65);
    for (BYTE& b : *payload) b = static_cast<BYTE>(b ^ 0xFF);
    return true;
}

bool WriteRate(HANDLE h, BYTE usbRate, BYTE freeRate) {
    BYTE f[21];
    memset(f, 0, sizeof(f));
    f[0] = 0x11;
    f[1] = 0x41;
    f[2] = usbRate;
    f[3] = freeRate;
    for (int i = 1; i < 21; ++i) f[i] = static_cast<BYTE>(f[i] ^ 0xFF);
    return HidD_SetFeature(h, f, 21) != FALSE;
}

}  // namespace

int main() {
    SetConsoleOutputCP(CP_UTF8);
    std::wstring path = FindIface(kMouseVid, kConfigPage);
    if (path.empty()) { std::printf("未找到鼠标。\n"); return 1; }

    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) { std::printf("打开失败 %lu\n", GetLastError()); return 1; }

    std::vector<BYTE> before;
    if (!ReadConfig(h, &before)) { std::printf("读取失败\n"); CloseHandle(h); return 1; }

    const BYTE b1 = before[1];
    const BYTE b2 = before[2];
    std::printf("初始: b1=0x%02X (低%u 高%u)  b2=0x%02X (低%u 高%u)\n",
                b1, b1 & 0xF, b1 >> 4, b2, b2 & 0xF, b2 >> 4);

    std::printf("写入 0x11 0x41: usbRate=%u freeRate=2 …\n", b1 & 0xF);
    if (!WriteRate(h, static_cast<BYTE>(b1 & 0xF), 2)) {
        std::printf("写入失败 %lu\n", GetLastError());
        CloseHandle(h);
        return 1;
    }
    Sleep(250);

    std::vector<BYTE> after;
    if (ReadConfig(h, &after)) {
        std::printf("写后: b1=0x%02X  b2=0x%02X\n", after[1], after[2]);
        if (after[1] != b1) std::printf("  → b1 发生变化 (0x%02X → 0x%02X)\n", b1, after[1]);
        if (after[2] != b2) std::printf("  → b2 发生变化 (0x%02X → 0x%02X)\n", b2, after[2]);
        if (after[1] == b1 && after[2] == b2) std::printf("  → 无变化\n");
    } else {
        std::printf("读回失败，仍恢复原值\n");
    }

    // 恢复
    std::printf("恢复: usbRate=%u freeRate=%u …\n", b1 & 0xF, b2 & 0xF);
    WriteRate(h, static_cast<BYTE>(b1 & 0xF), static_cast<BYTE>(b2 & 0xF));
    Sleep(300);

    std::vector<BYTE> finalState;
    if (ReadConfig(h, &finalState)) {
        std::printf("恢复后: b1=0x%02X  b2=0x%02X  %s\n", finalState[1], finalState[2],
                    (finalState[1] == b1 && finalState[2] == b2) ? "与初始一致 ✓" : "与初始不一致 ✗");
    }

    CloseHandle(h);
    return 0;
}
