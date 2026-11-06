// kb_led_write_test.cpp - 验证键盘灯效写入（cmd 35），改亮度后立即恢复
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
constexpr size_t kInfo = 9;

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
        HANDLE h = CreateFileW(d->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES a{};
        a.Size = sizeof(a);
        if (HidD_GetAttributes(h, &a) && a.VendorID == kVid && a.ProductID == kPid) {
            PHIDP_PREPARSED_DATA prep = nullptr;
            if (HidD_GetPreparsedData(h, &prep) && prep) {
                HIDP_CAPS c{};
                if (HidP_GetCaps(prep, &c) == HIDP_STATUS_SUCCESS && c.UsagePage == 0xFF60 &&
                    c.Usage == 0x0061) found = d->DevicePath;
                HidD_FreePreparsedData(prep);
            }
        }
        CloseHandle(h);
    }
    SetupDiDestroyDeviceInfoList(devs);
    return found;
}

bool Rd(HANDLE h, DWORD bytes, DWORD ms, std::vector<BYTE>* out) {
    std::vector<BYTE> buf(bytes, 0);
    OVERLAPPED ov{}; ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD read = 0;
    BOOL ok = ReadFile(h, buf.data(), bytes, &read, &ov);
    if (!ok) {
        if (GetLastError() != ERROR_IO_PENDING || WaitForSingleObject(ov.hEvent, ms) != WAIT_OBJECT_0 ||
            !GetOverlappedResult(h, &ov, &read, FALSE)) {
            CancelIo(h); CloseHandle(ov.hEvent); return false;
        }
    }
    CloseHandle(ov.hEvent);
    buf.resize(read); *out = std::move(buf);
    return read > 0;
}

bool Wr(HANDLE h, std::vector<BYTE>& buf) {
    OVERLAPPED ov{}; ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD written = 0;
    BOOL ok = WriteFile(h, buf.data(), static_cast<DWORD>(buf.size()), &written, &ov);
    if (!ok) {
        if (GetLastError() != ERROR_IO_PENDING || WaitForSingleObject(ov.hEvent, 500) != WAIT_OBJECT_0 ||
            !GetOverlappedResult(h, &ov, &written, FALSE)) { CloseHandle(ov.hEvent); return false; }
    }
    CloseHandle(ov.hEvent);
    return true;
}

bool ReadLed(HANDLE h, BYTE* led) {
    std::vector<BYTE> f(33, 0);
    f[0] = 0x00; f[1] = 0xAA; f[2] = 19; f[3] = 16;
    if (!Wr(h, f)) return false;
    Sleep(70);
    std::vector<BYTE> r;
    if (!Rd(h, 33, 800, &r) || r.size() < kInfo + 13) return false;
    for (int i = 0; i < 16; ++i) led[i] = r[kInfo + i];
    return true;
}

bool WriteLed(HANDLE h, const BYTE* led) {
    std::vector<BYTE> f(33, 0);
    f[0] = 0x00; f[1] = 0xAA; f[2] = 35; f[3] = 16;
    // 数据从 payload[8] 开始，即 frame[9]（frame[0] 是 ReportID）
    for (int i = 0; i < 16; ++i) f[9 + i] = led[i];
    return Wr(h, f);
}

}  // namespace

int main() {
    SetConsoleOutputCP(CP_UTF8);
    std::wstring path = FindIface();
    if (path.empty()) { std::printf("未找到键盘。\n"); return 1; }

    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE) { std::printf("打开失败 %lu\n", GetLastError()); return 1; }

    BYTE orig[16] = {};
    if (!ReadLed(h, orig)) { std::printf("读取灯效失败\n"); CloseHandle(h); return 1; }
    std::printf("当前: mode=%u rgb=(%u,%u,%u) sec=(%u,%u,%u) colorMode=%u bright=%u speed=%u\n",
                orig[0], orig[1], orig[2], orig[3], orig[5], orig[6], orig[7], orig[8], orig[9], orig[10]);

    BYTE target[16] = {};
    target[0] = 9;        // 恢复到写入前的 mode
    target[1] = 255; target[2] = 255; target[3] = 255;   // 白色
    target[4] = 255;      // 固定
    target[8] = 1;        // colorMode（写前值）
    target[9] = 5;        // brightness
    target[10] = 4;       // speed
    target[14] = 0xAA;
    target[15] = 0x55;

    std::printf("\n→ 写入：mode=9 白色 亮度=5 速度=4（还原）…\n");
    if (!WriteLed(h, target)) { std::printf("  写入失败\n"); CloseHandle(h); return 1; }
    Sleep(500);

    BYTE check[16] = {};
    if (ReadLed(h, check)) {
        std::printf("  写后: mode=%u rgb=(%u,%u,%u) bright=%u speed=%u type=%u\n",
                    check[0], check[1], check[2], check[3], check[9], check[10], check[12]);
        std::printf("  判定: %s\n",
                    (check[0] == 9 && check[1] == 255 && check[9] == 5)
                        ? "✓ 字段已正确落位" : "✗ 仍不匹配");
    }

    CloseHandle(h);
    return 0;
}
