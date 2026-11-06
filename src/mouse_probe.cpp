// mouse_probe.cpp - P0 实机验证：解析报告描述符 + 读取 A7 Pro 电量
//
// 目的：不依赖任何二手文档，直接从设备描述符得到每个 ReportID 的真实报文长度，
//       再用最小风险命令（读电量 0x11 0x06）验证帧格式假设：
//         · byte0 = ReportID（不取反）
//         · byte1..end = 命令与载荷，整段 ^0xFF
//         · 响应 byte1 = 命令 ^ 0xFF（用于匹配），byte2.. = 载荷 ^ 0xFF
//
// 编译：cl /std:c++17 /utf-8 /EHsc /MT /O2 mouse_probe.cpp /link setupapi.lib hid.lib

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <hidclass.h>

#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
#include <map>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")

#ifndef IOCTL_HID_GET_REPORT_DESCRIPTOR
#define IOCTL_HID_GET_REPORT_DESCRIPTOR CTL_CODE(0x0000000B, 0, 3, 0)
#endif

namespace {

constexpr USHORT kMchoseVid = 0x5253;
constexpr USAGE kConfigPage = 0xFF01;  // 命中配置 collection 的 usage page

struct ReportBits {
    unsigned inputBits = 0;
    unsigned outputBits = 0;
    unsigned featureBits = 0;
};

void ParseReportDescriptor(const std::vector<BYTE>& d, std::map<UINT8, ReportBits>& out) {
    int reportId = 0;
    int reportSize = 0;
    int reportCount = 0;

    size_t i = 0;
    while (i < d.size()) {
        BYTE prefix = d[i];
        if (prefix == 0xFE) {  // long item
            if (i + 2 >= d.size()) break;
            i += 3 + d[i + 1];
            continue;
        }
        int size = prefix & 0x03;
        if (size == 3) size = 4;
        int type = (prefix >> 2) & 0x03;
        int tag = (prefix >> 4) & 0x0F;
        if (i + 1 + size > d.size()) break;

        uint32_t value = 0;
        for (int k = 0; k < size; ++k) value |= static_cast<uint32_t>(d[i + 1 + k]) << (8 * k);
        size_t next = i + 1 + size;

        if (type == 1) {  // Global
            if (tag == 0x8) reportId = static_cast<int>(value);
            else if (tag == 0x7) reportSize = static_cast<int>(value);
            else if (tag == 0x9) reportCount = static_cast<int>(value);
        } else if (type == 0) {  // Main
            unsigned bits = static_cast<unsigned>(reportSize * reportCount);
            ReportBits& rb = out[static_cast<UINT8>(reportId)];
            if (tag == 0x8) rb.inputBits += bits;
            else if (tag == 0x9) rb.outputBits += bits;
            else if (tag == 0xB) rb.featureBits += bits;
        }
        i = next;
    }
}

unsigned ToBytes(unsigned bits) { return (bits + 7) / 8; }
std::wstring FindConfigInterface() {
    GUID hidGuid{};
    HidD_GetHidGuid(&hidGuid);
    HDEVINFO devs = SetupDiGetClassDevsW(
        &hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devs == INVALID_HANDLE_VALUE) return std::wstring();

    std::wstring found;
    for (DWORD index = 0; found.empty(); ++index) {
        SP_DEVICE_INTERFACE_DATA itf = {sizeof(itf)};
        if (!SetupDiEnumDeviceInterfaces(devs, nullptr, &hidGuid, index, &itf)) break;

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
        bool match = false;
        if (HidD_GetAttributes(h, &attr) && attr.VendorID == kMchoseVid) {
            PHIDP_PREPARSED_DATA prep = nullptr;
            if (HidD_GetPreparsedData(h, &prep) && prep != nullptr) {
                HIDP_CAPS caps{};
                if (HidP_GetCaps(prep, &caps) == HIDP_STATUS_SUCCESS &&
                    caps.UsagePage == kConfigPage) {
                    match = true;
                }
                HidD_FreePreparsedData(prep);
            }
        }
        CloseHandle(h);
        if (match) found = detail->DevicePath;
    }
    SetupDiDestroyDeviceInfoList(devs);
    return found;
}

void DumpHex(const char* label, const std::vector<BYTE>& buf) {
    std::printf("%s (%zu B):\n", label, buf.size());
    for (size_t i = 0; i < buf.size(); ++i) {
        std::printf("%02X ", buf[i]);
        if ((i + 1) % 16 == 0) std::printf("\n");
    }
    if (buf.size() % 16 != 0) std::printf("\n");
}

bool SendAndReceive(HANDLE h, UINT8 reportId, USHORT reportBytes, UINT8 cmd,
                    const std::vector<BYTE>& args, std::vector<BYTE>* response) {
    std::vector<BYTE> frame(reportBytes, 0);
    frame[0] = reportId;
    frame[1] = cmd;
    for (size_t k = 0; k < args.size() && (2 + k) < frame.size(); ++k) frame[2 + k] = args[k];
    for (size_t k = 1; k < frame.size(); ++k) frame[k] = static_cast<BYTE>(frame[k] ^ 0xFF);

    DumpHex("  -> send", frame);
    if (!HidD_SetFeature(h, frame.data(), static_cast<ULONG>(frame.size()))) {
        std::printf("  SetFeature 失败，错误码 %lu\n", GetLastError());
        return false;
    }
    Sleep(40);

    std::vector<BYTE> resp(reportBytes, 0);
    resp[0] = reportId;
    if (!HidD_GetFeature(h, resp.data(), static_cast<ULONG>(resp.size()))) {
        std::printf("  GetFeature 失败，错误码 %lu\n", GetLastError());
        return false;
    }
    DumpHex("  <- recv", resp);
    if (response != nullptr) *response = resp;
    return true;
}

}  // namespace

int main() {
    SetConsoleOutputCP(CP_UTF8);

    std::wstring path = FindConfigInterface();
    if (path.empty()) {
        std::printf("未找到 VID 0x%04X / usagePage 0x%04X 的接口。\n", kMchoseVid, kConfigPage);
        std::printf("请确认接收器已插入、鼠标已唤醒。\n");
        return 1;
    }
    std::wprintf(L"配置接口: %ls\n\n", path.c_str());

    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        std::printf("打开失败，错误码 %lu\n", GetLastError());
        return 1;
    }

    HIDP_CAPS caps{};
    PHIDP_PREPARSED_DATA prep = nullptr;
    if (HidD_GetPreparsedData(h, &prep) && prep != nullptr) {
        HidP_GetCaps(prep, &caps);
        HidD_FreePreparsedData(prep);
    }
    USHORT len11 = caps.FeatureReportByteLength;
    std::printf("==== 报文长度（HIDP_CAPS）====\n");
    std::printf("Input %u B / Output %u B / Feature %u B（均含 ReportID 字节）\n\n",
                caps.InputReportByteLength, caps.OutputReportByteLength,
                caps.FeatureReportByteLength);
    if (len11 == 0) {
        std::printf("Feature 报文长度为 0，无法继续。\n");
        CloseHandle(h);
        return 1;
    }

    std::printf("\n================ 读电量：0x11 0x06（短命令 21B） ================\n");
    for (int round = 1; round <= 3; ++round) {
        std::printf("\n-- 第 %d 次 --\n", round);
        std::vector<BYTE> resp;
        if (!SendAndReceive(h, 0x11, 21, 0x06, {}, &resp)) {
            std::printf("短命令失败，改用 65B 重试\n");
            if (!SendAndReceive(h, 0x11, len11, 0x06, {}, &resp)) {
                CloseHandle(h);
                return 1;
            }
        }
        BYTE echo = static_cast<BYTE>(resp[1] ^ 0xFF);
        std::printf("  回显 %s（0x%02X）\n", echo == 0x06 ? "通过" : "不符", echo);
        std::printf("  反码后 [2..13]: ");
        for (size_t i = 2; i < 14 && i < resp.size(); ++i)
            std::printf("%3u ", static_cast<BYTE>(resp[i] ^ 0xFF));
        std::printf("\n  原始   [2..13]: ");
        for (size_t i = 2; i < 14 && i < resp.size(); ++i)
            std::printf("%02X ", resp[i]);
        std::printf("\n");
        Sleep(200);
    }

    CloseHandle(h);
    return 0;
}
