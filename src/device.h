// device.h - 设备读取层（与 UI 无关，可单独测试）
//
// 封装 P0 阶段已验证的两条链路：
//   鼠标 Mchose A7 Pro  : VID 0x5253 / usagePage 0xFF01 / ReportID 0x11 / 21 B feature
//   键盘 EWEADN X87     : VID 0x0C45:0xFEFE / usagePage 0xFF60 / 32 B output+input
//
// 所有函数均为同步阻塞（数百 ms），调用方必须放到工作线程。

#pragma once
#include <windows.h>

#include <string>
#include <vector>

struct DeviceReading {
    bool mousePresent = false;  // HID 接口存在（不代表能应答）
    bool kbPresent = false;
    bool mouseOk = false;
    int mouseBattery = -1;   // 百分比；-1 表示未知
    int mouseCharge = -1;    // 充电状态原始值
    bool kbOk = false;
    int kbBattery = -1;
    int kbCharge = -1;
};

// 鼠标整包配置（0x12 0x67）。仅解析 2.4G 链路的活动档位。
struct MouseConfig {
    bool ok = false;
    int profileIndex = 0;
    int dpiIndex = -1;       // 活动 DPI 档（0..5）
    int rateIndex = -1;      // 2.4G 回报率索引（0..5）
    int usbRateIndex = -1;   // 有线回报率索引（改 2.4G 时需原样保留）
    int dpiList[6] = {};     // 六档原始 DPI 值
    int dpiCount = 0;        // 启用档数
    int debounce = 0;        // ms
    int sleepMin = 0;        // 分钟
};

// 一次性读取两只设备（任一失败不影响另一只）
DeviceReading ReadDevices();

// 读取鼠标整包配置；通道卡死时自动用 0x11 0x58 恢复一次
MouseConfig ReadMouseConfig();

// ---------- 配置备份与恢复（FR-T4） ----------

// 读取原始整包（63 字节，反码后）。读失败返回 false——此时禁止任何写操作。
bool ReadMouseConfigRaw(std::vector<BYTE>* payload);

// 首次备份：仅当 backup.mouse.bin 不存在时写入，保护出厂/首次状态
bool EnsureMouseBackup(const std::wstring& configDir, std::wstring* outPath);

// 从备份文件还原；写入后轮询回读确认（容忍 reserved 与 sensor bit0 的硬件自维护差异）
bool RestoreMouseConfig(const std::wstring& configDir);

// 整包写入（0x12 0x57），payload 需为 63 字节
bool WriteMouseConfigRaw(const std::vector<BYTE>& payload);

// 单命令写回报率（0x11 0x41）。写前强制读通，失败返回 false
bool WriteMouseRate(int usbRate, int freeRate);

// 单命令写活动 DPI 档（0x11 0x40）。六档数值取自当前配置，仅切换活动档并回读确认
bool WriteMouseDpi(int activeIndex);

// 备份文件路径
std::wstring MouseBackupPath(const std::wstring& configDir);
