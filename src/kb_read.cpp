// kb_read.cpp - 读取 X87 电量（E HUB 协议，0xFF60 通道）
//
// 请求帧（32 B，前置 1 B ReportID=0 后写入）：
//   [0]=0xAA  [1]=0x10(cmd 设备信息)  [2]=0x18(len=24)  [3..4]=addr LE  [5..31]=0
// 响应（33 B）：
//   [0]=0x00  [1]=0x55(应答)  [2]=cmd 回显  [3]=len  [4..7]=保留  [8]=包元数据
//   [9..] 设备信息区：+4..5 VID(LE) / +6..7 PID(LE) / +16 workMode / +17 电量 / +18 充电状态
//
// 编译：cl /std:c++17 /utf-8 /EHsc /MT /O2 kb_read.cpp /link setupapi.lib hid.lib

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
constexpr size_t kInfoOffset = 9;  // 响应中设备信息区起点

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
    std::wstring path = FindIface(0xFF60, 0x0061);
    if (path.empty()) {
        std::printf("未找到 0xFF60/0x0061 接口。\n");
        return 1;
    }

    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        std::printf("打开失败，错误码 %lu\n", GetLastError());
        return 1;
    }

    for (int round = 1; round <= 3; ++round) {
        std::vector<BYTE> frame(33, 0);
        frame[0] = 0x00;  // ReportID
        frame[1] = 0xAA;  // header
        frame[2] = 0x10;  // cmd: 设备信息
        frame[3] = 0x18;  // len
        frame[4] = 0x00;  // addr lo
        frame[5] = 0x00;  // addr hi

        if (!WriteAll(h, frame)) {
            std::printf("第 %d 次：写入失败\n", round);
            continue;
        }
        Sleep(80);

        std::vector<BYTE> resp;
        if (!ReadTimeout(h, 33, 900, &resp) || resp.size() < kInfoOffset + 19) {
            std::printf("第 %d 次：无有效响应\n", round);
            Sleep(300);
            continue;
        }

        const BYTE* info = resp.data() + kInfoOffset;
        USHORT vid = static_cast<USHORT>(info[4] | (info[5] << 8));
        USHORT pid = static_cast<USHORT>(info[6] | (info[7] << 8));
        BYTE workMode = info[16];
        BYTE battery = info[17];
        BYTE charge = info[18];

        std::printf("第 %d 次 | VID 0x%04X  PID 0x%04X  workMode %u  电量 %u%%  充电状态 %u\n",
                    round, vid, pid, workMode, battery, charge);
        Sleep(250);
    }

    CloseHandle(h);
    return 0;
}
