// kbd_listen.cpp - 判定 X87 归属于哪个 HID 键盘端点
//
// 做法：枚举所有 usage 0x0001/0x0006（标准键盘）接口，并行阻塞读取输入报告。
//       用户敲击 X87 时，哪个端点先吐出按键数据，哪个就是 X87 的链路。
//
// 编译：cl /std:c++17 /utf-8 /EHsc /MT /O2 kbd_listen.cpp /link setupapi.lib hid.lib
// 用法：kbd_listen.exe [--seconds N]   默认 60 秒

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
#include <thread>
#include <atomic>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")

namespace {

std::atomic<bool> g_stop{false};
CRITICAL_SECTION g_lock;
std::atomic<int> g_totalReports{0};

struct Target {
    std::wstring path;
    std::string tag;
    USHORT inLen = 0;
};

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::string ShortTag(const std::wstring& path) {
    std::string p = Narrow(path);
    size_t pos = p.find("vid_");
    if (pos == std::string::npos) return p.substr(0, 24);
    size_t end = p.find("#{", pos);
    std::string seg = p.substr(pos, end == std::string::npos ? 32 : end - pos);
    for (char& c : seg) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    return seg;
}

void Listen(const Target& t, int seconds) {
    HANDLE h = CreateFileW(t.path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        EnterCriticalSection(&g_lock);
        std::printf("[%s] 打开失败，错误码 %lu\n", t.tag.c_str(), GetLastError());
        LeaveCriticalSection(&g_lock);
        return;
    }

    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::vector<BYTE> buf(t.inLen > 1 ? t.inLen : 16);
    DWORD deadline = GetTickCount() + static_cast<DWORD>(seconds) * 1000;

    while (!g_stop.load() && GetTickCount() < deadline) {
        OVERLAPPED ov{};
        ov.hEvent = ev;
        ResetEvent(ev);
        DWORD read = 0;

        if (!ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &read, &ov)) {
            if (GetLastError() != ERROR_IO_PENDING) break;
            if (WaitForSingleObject(ev, 200) != WAIT_OBJECT_0) {
                CancelIo(h);
                continue;
            }
            if (!GetOverlappedResult(h, &ov, &read, FALSE)) break;
        }
        if (read == 0) continue;

        bool allZero = true;
        for (DWORD i = 0; i < read; ++i) {
            if (buf[i] != 0) { allZero = false; break; }
        }

        EnterCriticalSection(&g_lock);
        std::printf("[%s] %s", t.tag.c_str(), allZero ? "松开 " : "按键 ");
        for (DWORD i = 0; i < read; ++i) std::printf("%02X ", buf[i]);
        std::printf("\n");
        std::fflush(stdout);
        LeaveCriticalSection(&g_lock);

        if (!allZero) g_totalReports.fetch_add(1);
    }

    CloseHandle(ev);
    CloseHandle(h);
}

}  // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    InitializeCriticalSection(&g_lock);

    int seconds = 60;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--seconds" && i + 1 < argc) {
            seconds = std::atoi(argv[i + 1]);
        }
    }

    GUID hidGuid{};
    HidD_GetHidGuid(&hidGuid);
    HDEVINFO devs = SetupDiGetClassDevsW(
        &hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devs == INVALID_HANDLE_VALUE) {
        std::printf("枚举失败，错误码 %lu\n", GetLastError());
        return 1;
    }

    std::vector<Target> targets;
    for (DWORD index = 0;; ++index) {
        SP_DEVICE_INTERFACE_DATA itf = {sizeof(itf)};
        if (!SetupDiEnumDeviceInterfaces(devs, nullptr, &hidGuid, index, &itf)) break;

        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(devs, &itf, nullptr, 0, &need, nullptr);
        if (need == 0) continue;

        std::vector<BYTE> raw(need);
        auto* detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(raw.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(devs, &itf, detail, need, nullptr, nullptr)) continue;

        HANDLE probe = CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   nullptr, OPEN_EXISTING, 0, nullptr);
        if (probe == INVALID_HANDLE_VALUE) continue;

        PHIDP_PREPARSED_DATA prep = nullptr;
        if (HidD_GetPreparsedData(probe, &prep) && prep != nullptr) {
            HIDP_CAPS caps{};
            if (HidP_GetCaps(prep, &caps) == HIDP_STATUS_SUCCESS &&
                caps.UsagePage == 0x0001 && caps.Usage == 0x0006) {
                Target t;
                t.path = detail->DevicePath;
                t.tag = ShortTag(t.path);
                t.inLen = caps.InputReportByteLength;
                targets.push_back(std::move(t));
            }
            HidD_FreePreparsedData(prep);
        }
        CloseHandle(probe);
    }
    SetupDiDestroyDeviceInfoList(devs);

    if (targets.empty()) {
        std::printf("未找到任何标准键盘接口。\n");
        return 1;
    }

    std::printf("找到 %zu 个键盘端点，开始监听 %d 秒。\n", targets.size(), seconds);
    std::printf("请在前 20 秒内连续敲击 X87 上的几个按键。\n\n");
    Beep(1200, 200);

    std::vector<std::thread> threads;
    for (const auto& t : targets) {
        std::printf("  监听 %s（IN %u B）\n", t.tag.c_str(), t.inLen);
        threads.emplace_back(Listen, t, seconds);
    }
    for (auto& th : threads) th.join();

    Beep(800, 300);
    std::printf("\n================ 结果 ================\n");
    std::printf("期间共收到 %d 条非空按键报告。\n", g_totalReports.load());
    if (g_totalReports.load() == 0) {
        std::printf("全部端点均无输入：键盘可能未开机、未配对，或敲击时未被捕获。\n");
    } else {
        std::printf("有上方输出的端点即为 X87 实际链路。\n");
    }

    DeleteCriticalSection(&g_lock);
    return 0;
}
