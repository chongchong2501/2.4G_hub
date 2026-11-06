// device.cpp - HID 读取实现（零第三方依赖，仅 Windows SDK）
//
// 编译：cl /std:c++17 /utf-8 /EHsc /MT /O2 device.cpp

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "device.h"

#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>

#include <string>
#include <vector>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")

namespace {

constexpr USHORT kMouseVid = 0x5253;
constexpr USAGE kMousePage = 0xFF01;

constexpr USHORT kKbVid = 0x0C45;
constexpr USHORT kKbPid = 0xFEFE;
constexpr USAGE kKbPage = 0xFF60;
constexpr USAGE kKbUsage = 0x0061;

struct Iface {
    std::wstring path;
    USHORT inLen = 0;
    USHORT outLen = 0;
    USHORT featLen = 0;
};

// 按 VID(+PID) 与顶层 collection 的 usagePage/usage 定位接口
bool FindIface(USHORT vid, USHORT pid, USAGE page, USAGE usage, Iface* out) {
    GUID hidGuid{};
    HidD_GetHidGuid(&hidGuid);
    HDEVINFO devs = SetupDiGetClassDevsW(
        &hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devs == INVALID_HANDLE_VALUE) return false;

    bool found = false;
    for (DWORD i = 0; !found; ++i) {
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
        if (HidD_GetAttributes(h, &attr) && attr.VendorID == vid &&
            (pid == 0 || attr.ProductID == pid)) {
            PHIDP_PREPARSED_DATA prep = nullptr;
            if (HidD_GetPreparsedData(h, &prep) && prep != nullptr) {
                HIDP_CAPS caps{};
                if (HidP_GetCaps(prep, &caps) == HIDP_STATUS_SUCCESS &&
                    caps.UsagePage == page && (usage == 0 || caps.Usage == usage)) {
                    out->path = detail->DevicePath;
                    out->inLen = caps.InputReportByteLength;
                    out->outLen = caps.OutputReportByteLength;
                    out->featLen = caps.FeatureReportByteLength;
                    found = true;
                }
                HidD_FreePreparsedData(prep);
            }
        }
        CloseHandle(h);
    }
    SetupDiDestroyDeviceInfoList(devs);
    return found;
}

// 长报告通道卡死时，用重选 profile 恢复（协议文档记载的唯一方法）
bool RecoverMouseChannel(HANDLE h, BYTE profileIndex) {
    BYTE f[21];
    memset(f, 0, sizeof(f));
    f[0] = 0x11;
    f[1] = 0x58;
    f[2] = profileIndex;
    for (int i = 1; i < 21; ++i) f[i] = static_cast<BYTE>(f[i] ^ 0xFF);
    if (!HidD_SetFeature(h, f, 21)) return false;
    Sleep(150);
    return true;
}

// 读整包配置；返回 false 表示通道不可用（此时**禁止**任何写操作）
bool ReadConfigBlock(HANDLE h, std::vector<BYTE>* payload) {
    auto once = [&](std::vector<BYTE>* out) -> bool {
        BYTE frame[65];
        memset(frame, 0, sizeof(frame));
        frame[0] = 0x12;
        frame[1] = 0x67;
        for (int i = 1; i < 65; ++i) frame[i] = static_cast<BYTE>(frame[i] ^ 0xFF);
        if (!HidD_SetFeature(h, frame, 65)) return false;
        Sleep(90);

        BYTE resp[65] = {0x12};
        if (!HidD_GetFeature(h, resp, 65)) return false;
        if (static_cast<BYTE>(resp[1] ^ 0xFF) != 0x67) return false;

        out->assign(resp + 2, resp + 65);
        for (BYTE& b : *out) b = static_cast<BYTE>(b ^ 0xFF);

        for (BYTE b : *out) {
            if (b != 0) return true;   // 有非零数据才算有效
        }
        return false;                  // 全 0 = 通道卡死
    };

    if (once(payload)) return true;
    if (!RecoverMouseChannel(h, 0)) return false;
    return once(payload);
}

bool OverlappedRead(HANDLE h, DWORD bytes, DWORD timeoutMs, std::vector<BYTE>* out) {
    std::vector<BYTE> buf(bytes, 0);
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD read = 0;

    BOOL ok = ReadFile(h, buf.data(), bytes, &read, &ov);
    if (!ok) {
        if (GetLastError() != ERROR_IO_PENDING ||
            WaitForSingleObject(ov.hEvent, timeoutMs) != WAIT_OBJECT_0 ||
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

bool OverlappedWrite(HANDLE h, std::vector<BYTE>& buf) {
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

// 鼠标：0x11 0x06，载荷整体 ^0xFF，响应 byte1 为命令回显
bool ReadMouseBattery(DeviceReading* r) {
    Iface it;
    if (!FindIface(kMouseVid, 0, kMousePage, 0, &it)) return false;

    r->mousePresent = true;   // 接口存在，能否应答另说
    HANDLE h = CreateFileW(it.path.c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    bool ok = false;
    for (int attempt = 0; attempt < 4 && !ok; ++attempt) {
        BYTE frame[21];
        memset(frame, 0xFF, sizeof(frame));
        frame[0] = 0x11;              // ReportID 不取反
        frame[1] = 0x06 ^ 0xFF;       // 命令 0x06 取反

        if (!HidD_SetFeature(h, frame, sizeof(frame))) {
            Sleep(120);
            continue;
        }
        Sleep(40);

        BYTE resp[21] = {0x11};
        if (!HidD_GetFeature(h, resp, sizeof(resp))) {
            Sleep(120);
            continue;
        }
        if (static_cast<BYTE>(resp[1] ^ 0xFF) != 0x06) {
            Sleep(120);
            continue;
        }
        // 设备刚从休眠唤醒时，载荷会整体回 0xFF（反码后全 0），
        // 此时命令回显仍正确，但数据无效——必须重试，否则会把 0% 当成真实电量
        bool emptyPayload = true;
        for (int i = 2; i < 12; ++i) {
            if (resp[i] != 0xFF) {
                emptyPayload = false;
                break;
            }
        }
        if (emptyPayload) {
            Sleep(150);
            continue;
        }

        r->mouseOk = true;
        r->mouseBattery = static_cast<BYTE>(resp[2] ^ 0xFF);
        r->mouseCharge = static_cast<BYTE>(resp[3] ^ 0xFF);
        ok = true;
    }
    CloseHandle(h);
    return ok;
}

// 键盘：cmd 0x10 设备信息，信息区从 resp[9] 起（WebHID 与 Win32 差 1 字节 ReportID）
bool ReadKeyboardBattery(DeviceReading* r) {
    Iface it;
    if (!FindIface(kKbVid, kKbPid, kKbPage, kKbUsage, &it)) return false;

    r->kbPresent = true;
    HANDLE h = CreateFileW(it.path.c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    bool ok = false;
    for (int attempt = 0; attempt < 3 && !ok; ++attempt) {
        std::vector<BYTE> frame(33, 0);
        frame[0] = 0x00;  // ReportID
        frame[1] = 0xAA;  // header
        frame[2] = 0x10;  // cmd: 设备信息
        frame[3] = 0x18;  // len

        if (!OverlappedWrite(h, frame)) {
            Sleep(30);
            continue;
        }
        Sleep(60);

        std::vector<BYTE> resp;
        if (!OverlappedRead(h, 33, 800, &resp) || resp.size() < 9 + 19) {
            Sleep(30);
            continue;
        }
        if (resp[1] != 0x55) {
            Sleep(100);
            continue;
        }
        // 空响应保护：整段信息区为 0 时视为未就绪
        bool allZero = true;
        for (size_t i = 9; i < 20 && i < resp.size(); ++i) {
            if (resp[i] != 0) {
                allZero = false;
                break;
            }
        }
        if (allZero) {
            Sleep(120);
            continue;
        }
        const BYTE* info = resp.data() + 9;
        // 噪声帧校验：偶发返回 VID 0x0002 / 电量 0 的无效数据
        const USHORT vid = static_cast<USHORT>(info[4] | (info[5] << 8));
        if (vid != kKbVid) {
            Sleep(120);
            continue;
        }
        r->kbOk = true;
        r->kbBattery = info[17];
        r->kbCharge = info[18];
        ok = true;
    }
    CloseHandle(h);
    return ok;
}

}  // namespace

DeviceReading ReadDevices() {
    DeviceReading r;
    ReadMouseBattery(&r);
    ReadKeyboardBattery(&r);
    return r;
}

MouseConfig ReadMouseConfig() {
    MouseConfig c;
    Iface it;
    if (!FindIface(kMouseVid, 0, kMousePage, 0, &it)) return c;

    HANDLE h = CreateFileW(it.path.c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return c;

    std::vector<BYTE> p;
    if (ReadConfigBlock(h, &p) && p.size() >= 20) {
        c.profileIndex = p[0];

        // 实测（2026-11-06 探针）：高 nibble = Rate，低 nibble = DPI
        // 文档"Nibble order on the wire is usbRateIndex, usbDpiIndex, gRateIndex, gDpiIndex"
        // 指高→低排列，按低优先解析会读反
        c.rateIndex = (p[2] >> 4) & 0x0F;
        c.dpiIndex = p[2] & 0x0F;
        c.usbRateIndex = (p[1] >> 4) & 0x0F;

        for (int i = 0; i < 6; ++i) {
            c.dpiList[i] = p[4 + i * 2] | (p[5 + i * 2] << 8);
        }
        c.dpiCount = p[16];
        c.debounce = p[18];
        c.sleepMin = p[19];
        c.ok = true;
    }
    CloseHandle(h);
    return c;
}

// ---------- 配置备份与恢复 ----------

namespace {

// 整包写入 0x12 0x57。payload 63 字节，原样填入（含 reserved）
bool WriteConfigBlock(HANDLE h, const std::vector<BYTE>& payload) {
    BYTE frame[65];
    memset(frame, 0, sizeof(frame));
    frame[0] = 0x12;
    frame[1] = 0x57;
    for (size_t i = 0; i < payload.size() && (2 + i) < 65; ++i) {
        frame[2 + i] = payload[i];
    }
    for (int i = 1; i < 65; ++i) frame[i] = static_cast<BYTE>(frame[i] ^ 0xFF);
    if (!HidD_SetFeature(h, frame, 65)) return false;
    Sleep(40);  // 事务间 ≥25 ms
    return true;
}

// 写后轮询回读。容忍两处硬件自维护差异：offset 3 随 DPI 档翻转、sensor bit0 恒 1
bool VerifyConfigBlock(HANDLE h, const std::vector<BYTE>& expected) {
    for (int attempt = 0; attempt < 8; ++attempt) {
        Sleep(100);
        std::vector<BYTE> cur;
        if (!ReadConfigBlock(h, &cur) || cur.size() < expected.size()) continue;

        bool same = true;
        for (size_t k = 0; k < expected.size(); ++k) {
            if (k == 3) continue;
            BYTE a = expected[k];
            BYTE b = cur[k];
            if (k == 17) {
                a = static_cast<BYTE>(a & 0xFE);
                b = static_cast<BYTE>(b & 0xFE);
            }
            if (a != b) {
                same = false;
                break;
            }
        }
        if (same) return true;
    }
    return false;
}

}  // namespace

// ---------- 键盘灯效 ----------

namespace {

bool KbReadLedBlock(HANDLE h, BYTE* led) {
    std::vector<BYTE> f(33, 0);
    f[0] = 0x00;
    f[1] = 0xAA;
    f[2] = 19;   // cmd
    f[3] = 16;   // len
    if (!OverlappedWrite(h, f)) return false;
    Sleep(70);

    std::vector<BYTE> resp;
    if (!OverlappedRead(h, 33, 800, &resp) || resp.size() < 9 + 13) return false;
    for (int i = 0; i < 16; ++i) led[i] = resp[9 + i];
    return true;
}

bool KbWriteLedBlock(HANDLE h, const BYTE* led) {
    std::vector<BYTE> f(33, 0);
    f[0] = 0x00;   // ReportID
    f[1] = 0xAA;   // 帧头
    f[2] = 35;     // cmd
    f[3] = 16;     // len
    // 数据从 payload[8] 开始 → frame[9]
    for (int i = 0; i < 16; ++i) f[9 + i] = led[i];
    if (!OverlappedWrite(h, f)) return false;
    Sleep(60);
    return true;
}

}  // namespace

KeyboardLed ReadKeyboardLed() {
    KeyboardLed k;
    Iface it;
    if (!FindIface(kKbVid, kKbPid, kKbPage, kKbUsage, &it)) return k;

    HANDLE h = CreateFileW(it.path.c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE) return k;

    BYTE led[16] = {};
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (!KbReadLedBlock(h, led)) { Sleep(120); continue; }
        const int m = led[0];
        // 合法灯效模式：1..20 或 254
        if ((m >= 1 && m <= 20) || m == 254) break;
        Sleep(120);
    }
    if ((led[0] >= 1 && led[0] <= 20) || led[0] == 254) {
        k.ok = true;
        k.mode = led[0];
        k.r = led[1];
        k.g = led[2];
        k.b = led[3];
        k.colorMode = led[8];
        k.brightness = led[9];
        k.speed = led[10];
    }
    CloseHandle(h);
    return k;
}

bool WriteKeyboardLed(int mode, int brightness, int speed) {
    Iface it;
    if (!FindIface(kKbVid, kKbPid, kKbPage, kKbUsage, &it)) return false;

    HANDLE h = CreateFileW(it.path.c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    BYTE led[16] = {};
    if (!KbReadLedBlock(h, led)) {   // 写前必读，拿到其它字段原值
        CloseHandle(h);
        return false;
    }

    led[0] = static_cast<BYTE>(mode & 0xFF);
    led[4] = 255;
    led[9] = static_cast<BYTE>(brightness & 0xFF);
    led[10] = static_cast<BYTE>(speed & 0xFF);
    led[14] = 0xAA;
    led[15] = 0x55;

    // 灯效是动态的，读回值不可靠，只校验写入是否被接受
    const bool ok = KbWriteLedBlock(h, led);
    CloseHandle(h);
    return ok;
}

std::wstring MouseBackupPath(const std::wstring& configDir) {
    return configDir + L"\\backup.mouse.bin";
}

bool ReadMouseConfigRaw(std::vector<BYTE>* payload) {
    Iface it;
    if (!FindIface(kMouseVid, 0, kMousePage, 0, &it)) return false;
    HANDLE h = CreateFileW(it.path.c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const bool ok = ReadConfigBlock(h, payload);
    CloseHandle(h);
    return ok;
}

bool EnsureMouseBackup(const std::wstring& configDir, std::wstring* outPath) {
    const std::wstring path = MouseBackupPath(configDir);
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (outPath != nullptr) *outPath = path;
        return true;  // 已有备份，不覆盖（保护首次状态）
    }

    std::vector<BYTE> payload;
    if (!ReadMouseConfigRaw(&payload) || payload.size() < 60) return false;

    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok = WriteFile(f, payload.data(), static_cast<DWORD>(payload.size()), &written, nullptr);
    CloseHandle(f);
    if (!ok || written != payload.size()) {
        DeleteFileW(path.c_str());
        return false;
    }
    if (outPath != nullptr) *outPath = path;
    return true;
}

bool WriteMouseConfigRaw(const std::vector<BYTE>& payload) {
    if (payload.size() < 60) return false;

    Iface it;
    if (!FindIface(kMouseVid, 0, kMousePage, 0, &it)) return false;
    HANDLE h = CreateFileW(it.path.c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    // 安全铁律：通道读不通就绝不写（否则会把全 0 当成原值写回）
    std::vector<BYTE> probe;
    if (!ReadConfigBlock(h, &probe)) {
        CloseHandle(h);
        return false;
    }

    const bool ok = WriteConfigBlock(h, payload) && VerifyConfigBlock(h, payload);
    CloseHandle(h);
    return ok;
}

bool WriteMouseRate(int usbRate, int freeRate) {
    Iface it;
    if (!FindIface(kMouseVid, 0, kMousePage, 0, &it)) return false;
    HANDLE h = CreateFileW(it.path.c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    // 写前必读：通道卡死时拒绝写入，避免把全 0 当原值
    std::vector<BYTE> probe;
    if (!ReadConfigBlock(h, &probe)) {
        CloseHandle(h);
        return false;
    }

    BYTE f[21];
    memset(f, 0, sizeof(f));
    f[0] = 0x11;
    f[1] = 0x41;
    f[2] = static_cast<BYTE>(usbRate & 0x0F);
    f[3] = static_cast<BYTE>(freeRate & 0x0F);
    for (int i = 1; i < 21; ++i) f[i] = static_cast<BYTE>(f[i] ^ 0xFF);

    const bool ok = HidD_SetFeature(h, f, 21) != FALSE;
    CloseHandle(h);
    return ok;
}

bool WriteMouseDpi(int activeIndex) {
    if (activeIndex < 0 || activeIndex > 5) return false;

    Iface it;
    if (!FindIface(kMouseVid, 0, kMousePage, 0, &it)) return false;
    HANDLE h = CreateFileW(it.path.c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    // 写前必读：既校验通道，也拿到六档原始值
    std::vector<BYTE> cfg;
    if (!ReadConfigBlock(h, &cfg) || cfg.size() < 20) {
        CloseHandle(h);
        return false;
    }

    BYTE f[21];
    memset(f, 0, sizeof(f));
    f[0] = 0x11;
    f[1] = 0x40;
    f[2] = static_cast<BYTE>(activeIndex);  // usbDpiIndex
    f[3] = static_cast<BYTE>(activeIndex);  // gDpiIndex
    f[4] = 0;                               // reserved
    for (int i = 0; i < 6; ++i) {
        f[5 + i * 2] = cfg[4 + i * 2];
        f[6 + i * 2] = cfg[5 + i * 2];
    }
    f[17] = cfg[16];                        // dpiSum（启用档数，不能改）
    for (int i = 1; i < 21; ++i) f[i] = static_cast<BYTE>(f[i] ^ 0xFF);

    bool ok = HidD_SetFeature(h, f, 21) != FALSE;
    if (ok) {
        // 回读确认：低 nibble 为活动 DPI 档
        for (int attempt = 0; attempt < 8; ++attempt) {
            Sleep(120);
            std::vector<BYTE> cur;
            if (!ReadConfigBlock(h, &cur) || cur.size() < 3) continue;
            if ((cur[2] & 0x0F) == activeIndex) break;
            if (attempt == 7) ok = false;
        }
    }
    CloseHandle(h);
    return ok;
}

bool RestoreMouseConfig(const std::wstring& configDir) {
    const std::wstring path = MouseBackupPath(configDir);
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;

    const DWORD size = GetFileSize(f, nullptr);
    if (size < 60 || size > 128) {
        CloseHandle(f);
        return false;
    }

    std::vector<BYTE> payload(size);
    DWORD read = 0;
    const BOOL ok = ReadFile(f, payload.data(), size, &read, nullptr);
    CloseHandle(f);
    if (!ok || read != size) return false;

    return WriteMouseConfigRaw(payload);
}
