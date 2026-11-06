// kb_info.cpp - 试探 0xFF60 通道的设备信息帧（cmd=0x10），从中取电量
//
// 依据：E HUB 主包中设备信息读取为
//   s(device, {cmd:16, contentSize:56}) → 响应 {header:e[0], cmd:e[1], lenOrType:e[2], addr:e[3]|e[4]<<8, data:e.slice(8)}
//   解析后 batteryLevel = 数据区第 17 字节
// 通道：usagePage 0xFF60 / usage 0x0061，Output/Input 各 33 B（1 B ReportID + 32 B 帧）
//
// 编译：cl /std:c++17 /utf-8 /EHsc /MT /O2 kb_info.cpp /link setupapi.lib hid.lib

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

constexpr USHORT kKbVid = 0x0C45;
constexpr USHORT kKbPid = 0xFEFE;

std::wstring FindIface(USAGE page, USAGE usage) {
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
        if (HidD_GetAttributes(h, &a) && a.VendorID == kKbVid && a.ProductID == kKbPid) {
            PHIDP_PREPARSED_DATA prep = nullptr;
            if (HidD_GetPreparsedData(h, &prep) && prep) {
                HIDP_CAPS c{};
                if (HidP_GetCaps(prep, &c) == HIDP_STATUS_SUCCESS &&
                    c.UsagePage == page && c.Usage == usage) {
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

bool ReadTimeout(HANDLE h, DWORD bytes, DWORD ms, std::vector<BYTE>* out) {
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
            WaitForSingleObject(ov.hEvent, 800) != WAIT_OBJECT_0 ||
            !GetOverlappedResult(h, &ov, &written, FALSE)) {
            std::printf("    写入失败 err=%lu\n", GetLastError());
            CloseHandle(ov.hEvent);
            return false;
        }
    }
    CloseHandle(ov.hEvent);
    return true;
}

void Dump(const char* tag, const BYTE* p, size_t n) {
    std::printf("  %s", tag);
    for (size_t i = 0; i < n; ++i) std::printf("%02X ", p[i]);
    std::printf("\n");
}

}  // namespace

int main() {
    SetConsoleOutputCP(CP_UTF8);
    std::wstring path = FindIface(0xFF60, 0x0061);
    if (path.empty()) {
        std::printf("未找到 0xFF60/0x0061 接口。\n");
        return 1;
    }
    std::printf("通道: 0xFF60/0x0061（32 字节帧）\n\n");

    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        std::printf("打开失败，错误码 %lu\n", GetLastError());
        return 1;
    }

    struct Variant {
        const char* name;
        BYTE head;
        BYTE cmd;
        BYTE len;
        BYTE addrLo;
        BYTE addrHi;
        bool checksum;
    };
    const Variant variants[] = {
        {"A head=AA len=0x18", 0xAA, 0x10, 0x18, 0x00, 0x00, false},
        {"B head=AA len=0x00", 0xAA, 0x10, 0x00, 0x00, 0x00, false},
        {"C head=00 len=0x18", 0x00, 0x10, 0x18, 0x00, 0x00, false},
        {"D head=AA +sum", 0xAA, 0x10, 0x18, 0x00, 0x00, true},
        {"E head=5A len=0x18", 0x5A, 0x10, 0x18, 0x00, 0x00, false},
        {"F head=AA cmd=0x20", 0xAA, 0x20, 0x18, 0x00, 0x00, false},
    };

    for (const Variant& v : variants) {
        std::printf("---- %s ----\n", v.name);
        std::vector<BYTE> frame(32, 0);
        frame[0] = v.head;
        frame[1] = v.cmd;
        frame[2] = v.len;
        frame[3] = v.addrLo;
        frame[4] = v.addrHi;
        if (v.checksum) {
            unsigned sum = 0;
            for (size_t i = 0; i < 31; ++i) sum += frame[i];
            frame[31] = static_cast<BYTE>(sum & 0xFF);
        }

        std::vector<BYTE> wire(33, 0);
        wire[0] = 0x00;
        for (size_t i = 0; i < 32; ++i) wire[i + 1] = frame[i];

        Dump("->", wire.data(), wire.size());
        if (!WriteAll(h, wire)) continue;
        Sleep(90);

        std::vector<BYTE> resp;
        if (!ReadTimeout(h, 33, 900, &resp) || resp.empty()) {
            std::printf("  <- 无响应\n\n");
            continue;
        }
        Dump("<-", resp.data(), resp.size());
        const BYTE* p = resp.data();
        std::printf("     header=%02X cmd=%02X len=%02X addr=%02X%02X\n", p[0], p[1], p[2], p[4], p[3]);
        std::printf("     n[16..23] = ");
        for (int i = 24; i < 32 && i < static_cast<int>(resp.size()); ++i)
            std::printf("%02X(%u) ", p[i], p[i]);
        std::printf("\n\n");
        Sleep(250);
    }

    CloseHandle(h);
    return 0;
}
