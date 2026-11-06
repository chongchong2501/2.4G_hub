// kb_keymap_probe.cpp - 读取键盘主层按键映射（cmd 18，128 键 × 4 字节 = 512 B）
//
// 分包：每包 24 B 数据，addr 递增 24，共 ceil(512/24) = 22 包

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

constexpr USHORT kVid = 0x0C45;
constexpr USHORT kPid = 0xFEFE;
constexpr size_t kInfoOffset = 9;
constexpr size_t kPerPacket = 24;
constexpr size_t kTotal = 512;

std::wstring FindIface() {
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
        if (HidD_GetAttributes(h, &a) && a.VendorID == kVid && a.ProductID == kPid) {
            PHIDP_PREPARSED_DATA prep = nullptr;
            if (HidD_GetPreparsedData(h, &prep) && prep) {
                HIDP_CAPS c{};
                if (HidP_GetCaps(prep, &c) == HIDP_STATUS_SUCCESS && c.UsagePage == 0xFF60 &&
                    c.Usage == 0x0061) {
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

bool ReadWithTimeout(HANDLE h, DWORD bytes, DWORD ms, std::vector<BYTE>* out) {
    std::vector<BYTE> buf(bytes, 0);
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD read = 0;
    BOOL ok = ReadFile(h, buf.data(), bytes, &read, &ov);
    if (!ok) {
        if (GetLastError() != ERROR_IO_PENDING ||
            WaitForSingleObject(ov.hEvent, ms) != WAIT_OBJECT_0 ||
            !GetOverlappedResult(h, &ov, &read, FALSE)) {
            CancelIo(h);
            CloseHandle(ov.hEvent);
            return false;
        }
    }
    CloseHandle(ov.hEvent);
    buf.resize(read);
    *out = std::move(buf);
    return read > 0;
}

bool WriteAll(HANDLE h, std::vector<BYTE>& buf) {
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD written = 0;
    BOOL ok = WriteFile(h, buf.data(), static_cast<DWORD>(buf.size()), &written, &ov);
    if (!ok) {
        if (GetLastError() != ERROR_IO_PENDING ||
            WaitForSingleObject(ov.hEvent, 500) != WAIT_OBJECT_0 ||
            !GetOverlappedResult(h, &ov, &written, FALSE)) {
            CloseHandle(ov.hEvent);
            return false;
        }
    }
    CloseHandle(ov.hEvent);
    return true;
}

}  // namespace

int main() {
    SetConsoleOutputCP(CP_UTF8);
    std::wstring path = FindIface();
    if (path.empty()) { std::printf("未找到键盘接口。\n"); return 1; }

    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE) { std::printf("打开失败 %lu\n", GetLastError()); return 1; }

    std::vector<BYTE> all;
    all.reserve(kTotal);

    const int packets = static_cast<int>((kTotal + kPerPacket - 1) / kPerPacket);
    std::printf("读取主层按键映射：cmd 18，%d 包 × %zu B\n\n", packets, kPerPacket);

    for (int i = 0; i < packets; ++i) {
        const unsigned addr = static_cast<unsigned>(i * kPerPacket);

        std::vector<BYTE> frame(33, 0);
        frame[0] = 0x00;
        frame[1] = 0xAA;
        frame[2] = 18;
        frame[3] = static_cast<BYTE>(kPerPacket);
        frame[4] = static_cast<BYTE>(addr & 0xFF);
        frame[5] = static_cast<BYTE>((addr >> 8) & 0xFF);

        if (!WriteAll(h, frame)) { std::printf("包 %d 写入失败\n", i); break; }
        Sleep(60);

        std::vector<BYTE> resp;
        if (!ReadWithTimeout(h, 33, 800, &resp) || resp.size() < kInfoOffset + kPerPacket) {
            std::printf("包 %d 无响应\n", i);
            Sleep(150);
            continue;
        }

        const size_t take = std::min(kPerPacket, kTotal - all.size());
        all.insert(all.end(), resp.begin() + kInfoOffset,
                   resp.begin() + kInfoOffset + take);
        std::printf("  包 %2d addr=%3u ok\n", i, addr);
        Sleep(40);
    }

    std::printf("\n共取到 %zu 字节\n\n", all.size());
    if (all.size() < 4) { CloseHandle(h); return 1; }

    // 每 4 字节一个键
    std::printf("非零按键映射（index: pageType p1 p2 p3）：\n");
    int nonzero = 0;
    for (size_t k = 0; k + 3 < all.size(); k += 4) {
        const BYTE a = all[k], b = all[k + 1], c = all[k + 2], d = all[k + 3];
        if (a == 0 && b == 0 && c == 0 && d == 0) continue;
        std::printf("  key %3zu: %3u %3u %3u %3u\n", k / 4, a, b, c, d);
        if (++nonzero >= 100) { std::printf("  …（超过 100 项，截断）\n"); break; }
    }
    std::printf("\n非零按键数（前 128）: %d\n", nonzero);

    CloseHandle(h);
    return 0;
}
