// kb_led_probe.cpp - 读取键盘灯效（E HUB cmd 19，contentSize 16）
//
// 帧：AA <cmd> <len> <addrL> <addrH> ...  信息区从响应 resp[9] 起
// 字段：mode / r / g / b / - / r2 g2 b2 / colorMode / brightness / speed / direction / effectType

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
constexpr USAGE kPage = 0xFF60;
constexpr USAGE kUsage = 0x0061;

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
                if (HidP_GetCaps(prep, &c) == HIDP_STATUS_SUCCESS && c.UsagePage == kPage &&
                    c.Usage == kUsage) {
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

void Dump(const char* tag, const BYTE* p, size_t n) {
    std::printf("%s: ", tag);
    for (size_t i = 0; i < n; ++i) std::printf("%02X ", p[i]);
    std::printf("\n");
}

}  // namespace

int main() {
    SetConsoleOutputCP(CP_UTF8);
    std::wstring path = FindIface();
    if (path.empty()) {
        std::printf("未找到键盘接口。\n");
        return 1;
    }
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        std::printf("打开失败 %lu\n", GetLastError());
        return 1;
    }

    struct Req {
        BYTE cmd;
        BYTE len;
        const char* name;
    };
    const Req reqs[] = {
        {19, 16, "灯效 (getLEDEffect)"},
        {17, 1, "游戏模式"},
        {23, 16, "轴/回报率?"},
    };

    for (const Req& r : reqs) {
        std::printf("\n===== cmd %u  %s =====\n", r.cmd, r.name);

        std::vector<BYTE> frame(33, 0);
        frame[0] = 0x00;   // ReportID
        frame[1] = 0xAA;   // 帧头
        frame[2] = r.cmd;
        frame[3] = r.len;
        // frame[4], frame[5] = addr

        if (!WriteAll(h, frame)) {
            std::printf("  写入失败\n");
            continue;
        }
        Sleep(80);

        std::vector<BYTE> resp;
        if (!ReadWithTimeout(h, 33, 900, &resp) || resp.size() < 12) {
            std::printf("  无响应\n");
            continue;
        }
        Dump("  resp", resp.data(), resp.size());
        std::printf("  header=%02X status=%02X cmd=%02X len=%02X\n", resp[1], resp[2], resp[3], resp[4]);

        const BYTE* info = resp.data() + 9;
        std::printf("  info[0..15]: ");
        for (size_t i = 0; i < 16 && (9 + i) < resp.size(); ++i) std::printf("%02X ", info[i]);
        std::printf("\n");

        if (r.cmd == 19) {
            std::printf("  → mode=%u  RGB=(%u,%u,%u)  二级=(%u,%u,%u)\n",
                        info[0], info[1], info[2], info[3], info[5], info[6], info[7]);
            std::printf("    colorMode=%u brightness=%u speed=%u direction=%u effectType=%u\n",
                        info[8], info[9], info[10], info[11], info[12]);
        }
        Sleep(200);
    }

    CloseHandle(h);
    return 0;
}
