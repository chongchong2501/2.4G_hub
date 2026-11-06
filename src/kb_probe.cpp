// kb_probe.cpp - X87（0x0C45:0xFEFE）厂商通道探测
//
// 三个待判通道（来自 P0 枚举）：
//   A. usagePage 0xFFFF / usage 0x0001，IN 4B，ReportID 0x05  → 疑似设备状态/电量主动上报
//   B. usagePage 0xFF60 / usage 0x0061，IN/OUT 33B，ID 0     → QMK/VIA raw HID 特征签名
//   C. usagePage 0xFF59 / usage 0x0061，IN/OUT 65B，ID 0     → 厂商私有通道
//
// 探测策略（全部为只读，无写入设备配置）：
//   1. 监听通道 A 的输入 6 秒，看是否有周期性上报
//   2. 向 B / C 发送 QMK get_protocol_version（命令 0x01），看是否返回版本号
//
// 编译：cl /std:c++17 /utf-8 /EHsc /MT /O2 kb_probe.cpp /link setupapi.lib hid.lib

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

std::wstring FindInterface(USHORT vid, USHORT pid, USAGE page, USAGE usage) {
    GUID hidGuid{};
    HidD_GetHidGuid(&hidGuid);
    HDEVINFO devs = SetupDiGetClassDevsW(
        &hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devs == INVALID_HANDLE_VALUE) return std::wstring();

    std::wstring found;
    for (DWORD i = 0; found.empty(); ++i) {
        SP_DEVICE_INTERFACE_DATA itf = {sizeof(itf)};
        if (!SetupDiEnumDeviceInterfaces(devs, nullptr, &hidGuid, i, &itf)) break;

        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(devs, &itf, nullptr, 0, &need, nullptr);
        if (need == 0) continue;

        std::vector<BYTE> raw(need);
        auto* detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(raw.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(devs, &itf, detail, need, nullptr, nullptr)) continue;

        HANDLE h = CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;

        HIDD_ATTRIBUTES attr{};
        attr.Size = sizeof(attr);
        if (HidD_GetAttributes(h, &attr) && attr.VendorID == vid && attr.ProductID == pid) {
            PHIDP_PREPARSED_DATA prep = nullptr;
            if (HidD_GetPreparsedData(h, &prep) && prep != nullptr) {
                HIDP_CAPS caps{};
                if (HidP_GetCaps(prep, &caps) == HIDP_STATUS_SUCCESS &&
                    caps.UsagePage == page && (usage == 0 || caps.Usage == usage)) {
                    found = detail->DevicePath;
                }
                HidD_FreePreparsedData(prep);
            }
        }
        CloseHandle(h);
    }
    SetupDiDestroyDeviceInfoList(devs);
    return found;
}

void DumpHex(const char* label, const BYTE* p, size_t n) {
    std::printf("%s (%zu B): ", label, n);
    for (size_t i = 0; i < n; ++i) std::printf("%02X ", p[i]);
    std::printf("\n");
}

bool ReadWithTimeout(HANDLE h, DWORD bytes, DWORD ms, std::vector<BYTE>* out) {
    std::vector<BYTE> buf(bytes, 0);
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD read = 0;

    BOOL ok = ReadFile(h, buf.data(), bytes, &read, &ov);
    if (!ok) {
        if (GetLastError() != ERROR_IO_PENDING) {
            CloseHandle(ov.hEvent);
            return false;
        }
        if (WaitForSingleObject(ov.hEvent, ms) != WAIT_OBJECT_0) {
            CancelIo(h);
            CloseHandle(ov.hEvent);
            return false;
        }
        if (!GetOverlappedResult(h, &ov, &read, FALSE)) {
            CloseHandle(ov.hEvent);
            return false;
        }
    }
    CloseHandle(ov.hEvent);
    buf.resize(read);
    *out = std::move(buf);
    return read > 0;
}

// 通道 A：监听主动上报
void ListenStatus() {
    std::printf("\n==== 通道 A：0xFFFF/0x0001（IN 4B）主动上报 ====\n");
    std::wstring path = FindInterface(kKbVid, kKbPid, 0xFFFF, 0x0001);
    if (path.empty()) {
        std::printf("未找到该接口。\n");
        return;
    }
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        std::printf("打开失败，错误码 %lu\n", GetLastError());
        return;
    }

    int got = 0;
    DWORD deadline = GetTickCount() + 6000;
    while (GetTickCount() < deadline) {
        std::vector<BYTE> buf;
        if (ReadWithTimeout(h, 16, 500, &buf) && !buf.empty()) {
            DumpHex("  recv", buf.data(), buf.size());
            ++got;
        }
    }
    std::printf("  6 秒内收到 %d 帧%s\n", got, got ? "" : "（该通道无主动上报）");
    CloseHandle(h);
}

// 通道 B / C：QMK get_protocol_version
void TryQmk(USHORT page, USAGE usage, const char* name, DWORD reportBytes) {
    std::printf("\n==== 通道 %s：0x%04X/0x%04X（%lu B）QMK 探测 ====\n", name, page, usage, reportBytes);
    std::wstring path = FindInterface(kKbVid, kKbPid, page, usage);
    if (path.empty()) {
        std::printf("未找到该接口。\n");
        return;
    }
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        std::printf("打开失败，错误码 %lu\n", GetLastError());
        return;
    }

    std::vector<BYTE> req(reportBytes, 0);
    req[0] = 0x00;  // ReportID
    req[1] = 0x01;  // QMK id_get_protocol_version
    DumpHex("  -> send", req.data(), req.size());

    DWORD written = 0;
    OVERLAPPED wov{};
    wov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    BOOL wrote = WriteFile(h, req.data(), static_cast<DWORD>(req.size()), &written, &wov);
    if (!wrote) {
        if (GetLastError() != ERROR_IO_PENDING ||
            WaitForSingleObject(wov.hEvent, 500) != WAIT_OBJECT_0 ||
            !GetOverlappedResult(h, &wov, &written, FALSE)) {
            std::printf("  写入失败，错误码 %lu\n", GetLastError());
            CloseHandle(wov.hEvent);
            CloseHandle(h);
            return;
        }
    }
    CloseHandle(wov.hEvent);
    Sleep(60);

    std::vector<BYTE> resp;
    if (ReadWithTimeout(h, reportBytes, 800, &resp) && !resp.empty()) {
        DumpHex("  <- recv", resp.data(), resp.size());
        if (resp.size() >= 4 && resp[1] == 0x01) {
            unsigned ver = (static_cast<unsigned>(resp[2]) << 8) | resp[3];
            std::printf("  ** QMK/VIA 协议版本 = %u **\n", ver);
        } else {
            std::printf("  回包非 QMK 版本格式，需另行解析。\n");
        }
    } else {
        std::printf("  无响应（800 ms 超时），该通道未实现 QMK 协议。\n");
    }
    CloseHandle(h);
}

}  // namespace

int main() {
    SetConsoleOutputCP(CP_UTF8);
    std::printf("目标设备：VID 0x%04X / PID 0x%04X\n", kKbVid, kKbPid);
    std::printf("提示：若键盘已休眠，请先敲几下再继续。\n");

    ListenStatus();
    TryQmk(0xFF60, 0x0061, "B", 33);
    TryQmk(0xFF59, 0x0061, "C", 65);
    return 0;
}
