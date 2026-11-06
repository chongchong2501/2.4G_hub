// kb_setled.cpp - 设定键盘灯效模式/亮度/速度
// 用法：kb_setled.exe <mode> <brightness> <speed>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")

namespace {

constexpr USHORT kVid = 0x0C45, kPid = 0xFEFE;

std::wstring FindIface() {
    GUID g{}; HidD_GetHidGuid(&g);
    HDEVINFO d = SetupDiGetClassDevsW(&g, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (d == INVALID_HANDLE_VALUE) return L"";
    std::wstring found;
    for (DWORD i = 0; found.empty(); ++i) {
        SP_DEVICE_INTERFACE_DATA itf = {sizeof(itf)};
        if (!SetupDiEnumDeviceInterfaces(d, nullptr, &g, i, &itf)) break;
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(d, &itf, nullptr, 0, &need, nullptr);
        if (!need) continue;
        std::vector<BYTE> raw(need);
        auto* det = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(raw.data());
        det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(d, &itf, det, need, nullptr, nullptr)) continue;
        HANDLE h = CreateFileW(det->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES a{}; a.Size = sizeof(a);
        if (HidD_GetAttributes(h, &a) && a.VendorID == kVid && a.ProductID == kPid) {
            PHIDP_PREPARSED_DATA p = nullptr;
            if (HidD_GetPreparsedData(h, &p) && p) {
                HIDP_CAPS c{};
                if (HidP_GetCaps(p, &c) == HIDP_STATUS_SUCCESS && c.UsagePage == 0xFF60 &&
                    c.Usage == 0x0061) found = det->DevicePath;
                HidD_FreePreparsedData(p);
            }
        }
        CloseHandle(h);
    }
    SetupDiDestroyDeviceInfoList(d);
    return found;
}

bool Rd(HANDLE h, DWORD n, DWORD ms, std::vector<BYTE>* o) {
    std::vector<BYTE> b(n, 0);
    OVERLAPPED ov{}; ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD r = 0;
    BOOL ok = ReadFile(h, b.data(), n, &r, &ov);
    if (!ok) {
        if (GetLastError() != ERROR_IO_PENDING || WaitForSingleObject(ov.hEvent, ms) != WAIT_OBJECT_0 ||
            !GetOverlappedResult(h, &ov, &r, FALSE)) { CancelIo(h); CloseHandle(ov.hEvent); return false; }
    }
    CloseHandle(ov.hEvent); b.resize(r); *o = std::move(b); return r > 0;
}

bool Wr(HANDLE h, std::vector<BYTE>& b) {
    OVERLAPPED ov{}; ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD w = 0;
    BOOL ok = WriteFile(h, b.data(), (DWORD)b.size(), &w, &ov);
    if (!ok) {
        if (GetLastError() != ERROR_IO_PENDING || WaitForSingleObject(ov.hEvent, 500) != WAIT_OBJECT_0 ||
            !GetOverlappedResult(h, &ov, &w, FALSE)) { CloseHandle(ov.hEvent); return false; }
    }
    CloseHandle(ov.hEvent); return true;
}

}  // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    const int mode = argc > 1 ? std::atoi(argv[1]) : 18;
    const int bright = argc > 2 ? std::atoi(argv[2]) : 3;
    const int speed = argc > 3 ? std::atoi(argv[3]) : 2;

    std::wstring path = FindIface();
    if (path.empty()) { std::printf("未找到键盘\n"); return 1; }

    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE) { std::printf("打开失败 %lu\n", GetLastError()); return 1; }

    // 读当前 16 字节作为底稿
    std::vector<BYTE> f(33, 0);
    f[0] = 0x00; f[1] = 0xAA; f[2] = 19; f[3] = 16;
    BYTE led[16] = {};
    if (Wr(h, f)) {
        Sleep(70);
        std::vector<BYTE> r;
        if (Rd(h, 33, 800, &r) && r.size() >= 25) {
            for (int i = 0; i < 16; ++i) led[i] = r[9 + i];
        }
    }

    led[0] = static_cast<BYTE>(mode);
    led[4] = 255;
    led[9] = static_cast<BYTE>(bright);
    led[10] = static_cast<BYTE>(speed);
    led[14] = 0xAA;
    led[15] = 0x55;

    std::vector<BYTE> w(33, 0);
    w[0] = 0x00; w[1] = 0xAA; w[2] = 35; w[3] = 16;
    for (int i = 0; i < 16; ++i) w[9 + i] = led[i];

    if (!Wr(h, w)) { std::printf("写入失败\n"); CloseHandle(h); return 1; }
    std::printf("已设置 mode=%d brightness=%d speed=%d\n", mode, bright, speed);

    CloseHandle(h);
    return 0;
}
