// hid_probe.cpp - P0 设备枚举与能力探测（零第三方依赖，仅 Windows SDK）
//
// 用途：落实需求文档 §5.9 P0 阶段——枚举本机 HID 设备，输出
//       VID/PID、制造与产品字符串、顶层 collection 的 UsagePage/Usage、
//       三种报文的长度与 ReportID 集合，用于确认：
//         · 迈从 A7 Pro 的 vendor collection（预期 0x5253 / usagePage 0xFF01|0xFF0B）
//         · 前行者 X87 是否落在 Evision 平台（预期 0x320F / usagePage 0xFF1C usage 0x92）
//
// 编译：cl /std:c++17 /utf-8 /EHsc /MT /O2 /W4 hid_probe.cpp /link setupapi.lib hid.lib
// 用法：hid_probe.exe          仅显示目标设备与 vendor 类 collection
//       hid_probe.exe --all    显示本机全部 HID 接口

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
#include <set>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")

namespace {

constexpr USHORT kMchoseVid = 0x5253;   // 迈从 A7 家族
constexpr USHORT kEvisionVid = 0x320F;  // Evision 公版平台（待确认）

std::string ToUtf8(const wchar_t* w) {
    if (w == nullptr || *w == L'\0') return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return std::string();
    std::string s(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

using HidStringFn = BOOLEAN(WINAPI*)(HANDLE, PVOID, ULONG);

std::string ReadHidString(HANDLE h, HidStringFn fn) {
    wchar_t buf[512] = {};
    if (fn(h, buf, static_cast<ULONG>(sizeof(buf)))) return ToUtf8(buf);
    return std::string();
}

struct Dev {
    std::wstring path;
    USHORT vid = 0;
    USHORT pid = 0;
    USHORT ver = 0;
    std::string mfr;
    std::string prod;
    std::string serial;
    USAGE usagePage = 0;
    USAGE usage = 0;
    USHORT inLen = 0;
    USHORT outLen = 0;
    USHORT featLen = 0;
    std::set<UINT8> idsIn;
    std::set<UINT8> idsOut;
    std::set<UINT8> idsFeat;
    bool opened = false;
    bool capsOk = false;
    DWORD openErr = 0;
};

void CollectIds(PHIDP_PREPARSED_DATA prep, HIDP_REPORT_TYPE type, std::set<UINT8>& out) {
    USHORT nValue = 256;
    std::vector<HIDP_VALUE_CAPS> values(nValue);
    if (HidP_GetValueCaps(type, values.data(), &nValue, prep) == HIDP_STATUS_SUCCESS) {
        for (USHORT i = 0; i < nValue; ++i) out.insert(values[i].ReportID);
    }
    USHORT nButton = 256;
    std::vector<HIDP_BUTTON_CAPS> buttons(nButton);
    if (HidP_GetButtonCaps(type, buttons.data(), &nButton, prep) == HIDP_STATUS_SUCCESS) {
        for (USHORT i = 0; i < nButton; ++i) out.insert(buttons[i].ReportID);
    }
}

std::string FormatIds(const std::set<UINT8>& ids) {
    if (ids.empty()) return "-";
    std::string s;
    char buf[8];
    for (UINT8 id : ids) {
        std::snprintf(buf, sizeof(buf), "%s0x%02X", s.empty() ? "" : ",", id);
        s += buf;
    }
    return s;
}

bool IsVendorPage(USAGE page) { return page >= 0xFF00; }

const char* MatchTag(const Dev& d) {
    if (d.vid == kMchoseVid) return "MCHOSE (A7 family)";
    if (d.vid == kEvisionVid) return "EVISION (X87 candidate)";
    return nullptr;
}

}  // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);

    bool showAll = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--all") showAll = true;
    }

    GUID hidGuid{};
    HidD_GetHidGuid(&hidGuid);

    HDEVINFO devs = SetupDiGetClassDevsW(
        &hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devs == INVALID_HANDLE_VALUE) {
        std::printf("SetupDiGetClassDevs 失败，错误码 %lu\n", GetLastError());
        return 1;
    }

    std::vector<Dev> all;
    for (DWORD index = 0;; ++index) {
        SP_DEVICE_INTERFACE_DATA itf = {sizeof(itf)};
        if (!SetupDiEnumDeviceInterfaces(devs, nullptr, &hidGuid, index, &itf)) {
            if (GetLastError() != ERROR_NO_MORE_ITEMS) {
                std::printf("枚举中断，错误码 %lu\n", GetLastError());
            }
            break;
        }

        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(devs, &itf, nullptr, 0, &need, nullptr);
        if (need == 0) continue;

        std::vector<BYTE> raw(need);
        auto* detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(raw.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(devs, &itf, detail, need, nullptr, nullptr)) continue;

        Dev d;
        d.path = detail->DevicePath;

        HANDLE h = CreateFileW(d.path.c_str(), GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            d.openErr = GetLastError();
            h = CreateFileW(d.path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr, OPEN_EXISTING, 0, nullptr);
        }

        if (h != INVALID_HANDLE_VALUE) {
            d.opened = true;
            HIDD_ATTRIBUTES attr{};
            attr.Size = sizeof(attr);
            if (HidD_GetAttributes(h, &attr)) {
                d.vid = attr.VendorID;
                d.pid = attr.ProductID;
                d.ver = attr.VersionNumber;
            }
            d.mfr = ReadHidString(h, HidD_GetManufacturerString);
            d.prod = ReadHidString(h, HidD_GetProductString);
            d.serial = ReadHidString(h, HidD_GetSerialNumberString);

            PHIDP_PREPARSED_DATA prep = nullptr;
            if (HidD_GetPreparsedData(h, &prep) && prep != nullptr) {
                HIDP_CAPS caps{};
                if (HidP_GetCaps(prep, &caps) == HIDP_STATUS_SUCCESS) {
                    d.capsOk = true;
                    d.usagePage = caps.UsagePage;
                    d.usage = caps.Usage;
                    d.inLen = caps.InputReportByteLength;
                    d.outLen = caps.OutputReportByteLength;
                    d.featLen = caps.FeatureReportByteLength;
                    CollectIds(prep, HidP_Input, d.idsIn);
                    CollectIds(prep, HidP_Output, d.idsOut);
                    CollectIds(prep, HidP_Feature, d.idsFeat);
                }
                HidD_FreePreparsedData(prep);
            }
            CloseHandle(h);
        }
        all.push_back(std::move(d));
    }
    SetupDiDestroyDeviceInfoList(devs);

    std::vector<const Dev*> targets;
    for (const Dev& d : all) {
        if (showAll || MatchTag(d) != nullptr || IsVendorPage(d.usagePage)) targets.push_back(&d);
    }

    std::printf("================ HID 枚举结果 ================\n");
    std::printf("接口总数 %zu，本次显示 %zu 个%s\n\n", all.size(), targets.size(),
                showAll ? "（--all）" : "（目标设备与 vendor collection）");

    if (targets.empty()) {
        std::printf("未发现任何 vendor collection 设备。\n"
                    "请确认：接收器已插入、设备已切到 2.4G 或有线模式、未被厂商驱动独占。\n\n");
    }

    int seq = 0;
    for (const Dev* pd : targets) {
        const Dev& d = *pd;
        ++seq;
        std::printf("[%d] %s\n", seq, MatchTag(d) ? MatchTag(d) : "USB HID");
        std::printf("    VID/PID  : 0x%04X / 0x%04X   (bcd 0x%04X)\n", d.vid, d.pid, d.ver);
        std::printf("    Name     : %s%s%s\n",
                    d.mfr.empty() ? "(无)" : d.mfr.c_str(),
                    d.prod.empty() ? "" : " / ",
                    d.prod.c_str());
        if (d.capsOk) {
            std::printf("    Usage    : page 0x%04X  usage 0x%04X%s\n", d.usagePage, d.usage,
                        IsVendorPage(d.usagePage) ? "  [vendor]" : "");
            std::printf("    Reports  : IN %u B / OUT %u B / FEAT %u B\n",
                        d.inLen, d.outLen, d.featLen);
            std::printf("    ReportID : IN{%s}  OUT{%s}  FEAT{%s}\n",
                        FormatIds(d.idsIn).c_str(), FormatIds(d.idsOut).c_str(),
                        FormatIds(d.idsFeat).c_str());
        } else {
            std::printf("    Usage    : 读取失败（%s）\n",
                        d.opened ? "preparsed data 不可用" : "设备打开失败");
            if (!d.opened) {
                std::printf("    Open     : 失败，错误码 %lu（可能被系统或厂商驱动独占）\n", d.openErr);
            }
        }
        if (!d.serial.empty()) std::printf("    Serial   : %s\n", d.serial.c_str());
        std::wprintf(L"    Path     : %ls\n", d.path.c_str());
        std::printf("\n");
    }

    int mchose = 0;
    int evision = 0;
    for (const Dev& d : all) {
        if (d.vid == kMchoseVid) ++mchose;
        if (d.vid == kEvisionVid) ++evision;
    }

    std::printf("================ 结论 ================\n");
    std::printf("Mchose  (0x%04X) 接口数：%d  %s\n", kMchoseVid, mchose,
                mchose ? "命中" : "未发现");
    std::printf("Evision (0x%04X) 接口数：%d  %s\n", kEvisionVid, evision,
                evision ? "命中" : "未发现");
    if (evision > 0) {
        std::printf("→ X87 若在上述接口中，请进一步比对 usagePage 0xFF1C / usage 0x0092。\n");
    } else {
        std::printf("→ 未命中 0x320F，键盘可能需要开通有线/2.4G 或走 §5.7 逆向流程。\n");
    }
    return 0;
}
