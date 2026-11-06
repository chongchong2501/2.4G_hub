// kb_battery.cpp - 用 E HUB 逆向出的 64 字节帧读取 X87 电量
//
// 帧结构（来自 new-hub.eweadn.cn 主包 main.59124...js）：
//   byte0  commandId          （电量命令 = 135 = 0x87）
//   byte1  commandParam
//   byte3  totalPacketLength
//   byte4  currentPacketIndex
//   byte5  length
//   byte62 checksum = 255 - (sum(bytes) & 0xFF)，计算前校验位置 0
// 传输：usagePage 0xFF59 / usage 0x0061，Output/Input 各 65 B（1 B ReportID + 64 B 帧）
//
// 编译：cl /std:c++17 /utf-8 /EHsc /MT /O2 kb_battery.cpp /link setupapi.lib hid.lib

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
constexpr size_t kChecksumIndex = 62;

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

        HANDLE h = CreateFileW(d->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, 0, nullptr);
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

void Dump(const char* tag, const BYTE* p, size_t n) {
    std::printf("%s: ", tag);
    for (size_t i = 0; i < n; ++i) std::printf("%02X ", p[i]);
    std::printf("\n");
}

BYTE Crc(const std::vector<BYTE>& frame) {
    unsigned sum = 0;
    for (size_t i = 0; i < frame.size(); ++i) {
        if (i == kChecksumIndex) continue;
        sum += frame[i];
    }
    return static_cast<BYTE>(255 - (sum & 0xFF));
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
            std::printf("  写入失败，错误码 %lu\n", GetLastError());
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
    std::wstring path = FindIface(0xFF59, 0x0061);
    if (path.empty()) {
        std::printf("未找到 0xFF59/0x0061 接口。\n");
        return 1;
    }
    std::printf("通道: 0xFF59/0x0061\n\n");

    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        std::printf("打开失败，错误码 %lu\n", GetLastError());
        return 1;
    }

    struct Variant {
        const char* name;
        BYTE b1;
        BYTE b3;
        BYTE b4;
        BYTE b5;
    };
    const Variant variants[] = {
        {"A: total=1 cur=0 len=0", 0, 1, 0, 0},
        {"B: total=0 cur=0 len=0", 0, 0, 0, 0},
        {"C: param=1 total=1", 1, 1, 0, 0},
        {"D: param=1 total=0", 1, 0, 0, 0},
    };

    for (const Variant& v : variants) {
        std::printf("---- %s ----\n", v.name);

        std::vector<BYTE> frame(64, 0);
        frame[0] = 0x87;
        frame[1] = v.b1;
        frame[3] = v.b3;
        frame[4] = v.b4;
        frame[5] = v.b5;
        frame[kChecksumIndex] = Crc(frame);

        std::vector<BYTE> wire(65, 0);
        wire[0] = 0x00;
        for (size_t i = 0; i < 64; ++i) wire[i + 1] = frame[i];

        Dump("  -> ", wire.data(), wire.size());
        if (!WriteAll(h, wire)) continue;
        Sleep(80);

        std::vector<BYTE> resp;
        if (!ReadTimeout(h, 65, 900, &resp) || resp.empty()) {
            std::printf("  <- 无响应\n\n");
            continue;
        }
        Dump("  <- ", resp.data(), resp.size());

        const BYTE* p = resp.data() + 1;
        std::printf("  payload[0..8] = %02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
                    p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8]);
        std::printf("  候选电量: p[6]=%u%%, p[7]=%u%%, p[8]=%u%%\n\n",
                    (unsigned)p[6], (unsigned)p[7], (unsigned)p[8]);
        Sleep(200);
    }

    CloseHandle(h);
    return 0;
}
