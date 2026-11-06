// mouse_packet.cpp - 读取并解析 A7 Pro 整包配置（0x12 0x67）
//
// 帧格式（沿用 0x11 已验证的规则）：
//   byte0 = ReportID（不取反）
//   byte1..end = 命令与载荷，整段 ^0xFF
// 响应：byte1 = 命令 ^ 0xFF（回显），byte2.. = 载荷 ^ 0xFF
//
// 目的：不照搬开源实现的偏移，直接从本机 dump 出真实布局，
//      再用"候选扫描"标出可能属于 DPI / 回报率 / profile 的字段。
//
// 编译：cl /std:c++17 /utf-8 /EHsc /MT /O2 mouse_packet.cpp /link setupapi.lib hid.lib

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

void HexDump(const char* tag, const BYTE* p, size_t n) {
    std::printf("%s (%zu B)\n", tag, n);
    for (size_t i = 0; i < n; ++i) {
        if (i % 16 == 0) std::printf("  %02zX: ", i);
        std::printf("%02X ", p[i]);
        if (i % 16 == 15) std::printf("\n");
    }
    if (n % 16 != 0) std::printf("\n");
}

bool ReadPacket(HANDLE h, std::vector<BYTE>* payload) {
    BYTE frame[65];
    memset(frame, 0, sizeof(frame));
    frame[0] = 0x12;   // ReportID
    frame[1] = 0x67;   // 读整包配置
    for (int i = 1; i < 65; ++i) frame[i] = static_cast<BYTE>(frame[i] ^ 0xFF);

    if (!HidD_SetFeature(h, frame, 65)) {
        std::printf("SetFeature 失败，错误码 %lu\n", GetLastError());
        return false;
    }
    Sleep(90);

    BYTE resp[65] = {0x12};
    if (!HidD_GetFeature(h, resp, 65)) {
        std::printf("GetFeature 失败，错误码 %lu\n", GetLastError());
        return false;
    }
    if (static_cast<BYTE>(resp[1] ^ 0xFF) != 0x67) {
        std::printf("命令回显不符：0x%02X\n", static_cast<BYTE>(resp[1] ^ 0xFF));
        return false;
    }

    payload->assign(resp + 2, resp + 65);
    for (BYTE& b : *payload) b = static_cast<BYTE>(b ^ 0xFF);
    return true;
}

// 长报告通道卡死时的恢复：重选 profile（文档记载的唯一方法）
bool RecoverChannel(HANDLE h, BYTE profileIndex) {
    BYTE f[21];
    memset(f, 0, sizeof(f));
    f[0] = 0x11;
    f[1] = 0x58;
    f[2] = profileIndex;
    for (int i = 1; i < 21; ++i) f[i] = static_cast<BYTE>(f[i] ^ 0xFF);
    if (!HidD_SetFeature(h, f, 21)) {
        std::printf("恢复命令发送失败，错误码 %lu\n", GetLastError());
        return false;
    }
    Sleep(150);
    return true;
}

}  // namespace

int main() {
    SetConsoleOutputCP(CP_UTF8);
    std::wstring path = FindIface(kMouseVid, kConfigPage);
    if (path.empty()) {
        std::printf("未找到鼠标配置接口。\n");
        return 1;
    }
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        std::printf("打开失败，错误码 %lu\n", GetLastError());
        return 1;
    }

    std::vector<BYTE> payload;
    if (!ReadPacket(h, &payload)) {
        CloseHandle(h);
        return 1;
    }
    HexDump("payload（反码后，64 B，偏移 = 报文 - 2）", payload.data(), payload.size());

    bool allZero = true;
    for (BYTE b : payload) {
        if (b != 0) { allZero = false; break; }
    }
    if (allZero) {
        std::printf("\n整包全 0：长命令通道卡死，尝试 0x11 0x58 重选 profile 恢复…\n");
        if (RecoverChannel(h, 0) && ReadPacket(h, &payload)) {
            allZero = true;
            for (BYTE b : payload) {
                if (b != 0) { allZero = false; break; }
            }
        }
        if (allZero) {
            std::printf("恢复后仍为全 0，放弃。\n");
            CloseHandle(h);
            return 2;
        }
        std::printf("通道已恢复。\n");
        HexDump("payload（恢复后）", payload.data(), payload.size());
    }

    std::printf("\n===== 候选字段扫描 =====\n");

    std::printf("[u16 小端，100..32000 且为 50 的倍数 —— 疑似 DPI]\n");
    for (size_t i = 0; i + 1 < payload.size(); ++i) {
        unsigned v = payload[i] | (payload[i + 1] << 8);
        if (v >= 100 && v <= 32000 && v % 50 == 0) {
            std::printf("  +%02zX = %u\n", i, v);
        }
    }

    std::printf("[单字节 0..5 —— 疑似回报率索引]\n");
    for (size_t i = 0; i < payload.size(); ++i) {
        if (payload[i] <= 5) std::printf("  +%02zX = %u\n", i, payload[i]);
    }

    std::printf("[单字节 0..3 —— 疑似 profile / 档位]\n");
    for (size_t i = 0; i < payload.size(); ++i) {
        if (payload[i] <= 3) std::printf("  +%02zX = %u\n", i, payload[i]);
    }

    std::printf("\n===== 原始按字读取（每行 8 个 u16） =====\n");
    for (size_t i = 0; i + 1 < payload.size() && i < 32; i += 2) {
        std::printf("  +%02zX: %5u", i, payload[i] | (payload[i + 1] << 8));
        if ((i / 2) % 4 == 3) std::printf("\n");
    }
    std::printf("\n");

    std::printf("\n===== 按 MCHOSE 协议文档解析 =====\n");
    std::printf("profileIndex = %u\n", payload[0]);
    {
        const unsigned b1 = payload[1];
        const unsigned b2 = payload[2];
        std::printf("nibble 字节: b1=0x%02X  b2=0x%02X\n", b1, b2);
        std::printf("  文档读序 (低→高): usbRate=%u usbDpi=%u gRate=%u gDpi=%u\n",
                    b1 & 0x0F, (b1 >> 4) & 0x0F, b2 & 0x0F, (b2 >> 4) & 0x0F);
        std::printf("  反序假设        : gDpi=%u gRate=%u usbDpi=%u usbRate=%u\n",
                    b1 & 0x0F, (b1 >> 4) & 0x0F, b2 & 0x0F, (b2 >> 4) & 0x0F);
    }

    std::printf("\nDPI 六档: ");
    for (int i = 0; i < 6; ++i) {
        std::printf("%u ", payload[4 + i * 2] | (payload[5 + i * 2] << 8));
    }
    std::printf("\ndpiSum(启用档数) = %u\n", payload[16]);
    std::printf("sensor = 0x%02X  keyDebounce = %u ms  sleep = %u min\n",
                payload[17], payload[18], payload[19]);

    static const char* kRateNames[] = {"125Hz", "500Hz", "1000Hz", "2000Hz", "4000Hz", "8000Hz"};
    auto rateName = [](int idx) -> const char* {
        return (idx >= 0 && idx < 6) ? kRateNames[idx] : "?";
    };
    {
        const int gDpi = (payload[2] >> 4) & 0x0F;
        const int gRate = payload[2] & 0x0F;
        const int usbDpi = (payload[1] >> 4) & 0x0F;
        const int usbRate = payload[1] & 0x0F;
        std::printf("\n按文档读序推导 2.4G 侧: DPI 档 %d, 回报率档 %d (%s)\n",
                    gDpi, gRate, rateName(gRate));
        std::printf("按文档读序推导 有线侧: DPI 档 %d, 回报率档 %d (%s)\n",
                    usbDpi, usbRate, rateName(usbRate));
    }

    CloseHandle(h);
    return 0;
}
