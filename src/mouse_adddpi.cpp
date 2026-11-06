// mouse_adddpi.cpp - 直接增删 DPI 档位，用于隔离验证 SetMouseDpiStages
// 用法：mouse_adddpi.exe add | del
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "device.h"

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    const bool add = (argc > 1 && std::strcmp(argv[1], "del") != 0);

    std::vector<BYTE> cfg;
    if (!ReadMouseConfigRaw(&cfg)) { std::printf("读取失败\n"); return 1; }

    const int count = cfg[16];
    std::printf("当前 dpiSum=%d  dpi=", count);
    for (int i = 0; i < 6; ++i) std::printf("%d ", cfg[4 + i * 2] | (cfg[5 + i * 2] << 8));
    std::printf(" 活动档=%d\n", cfg[2] & 0x0F);

    if (count < 1 || count > 6) { std::printf("dpiSum 异常\n"); return 1; }

    int vals[6] = {};
    for (int i = 0; i < count; ++i) vals[i] = cfg[4 + i * 2] | (cfg[5 + i * 2] << 8);

    int newCount = count;
    int active = cfg[2] & 0x0F;

    if (add) {
        if (count >= 6) { std::printf("已达 6 档上限\n"); return 1; }
        int next = vals[count - 1] * 2;
        if (next > 26000) next = 26000;
        next = (next / 50) * 50;
        if (next < 100) next = 100;
        vals[count] = next;
        newCount = count + 1;
    } else {
        if (count <= 1) { std::printf("至少保留 1 档\n"); return 1; }
        newCount = count - 1;
        if (active >= newCount) active = newCount - 1;
    }
    if (active < 0) active = 0;

    std::printf("→ 写入 %d 档（活动档 %d）\n", newCount, active);
    const bool ok = SetMouseDpiStages(vals, newCount, active);
    std::printf("  SetMouseDpiStages = %s\n", ok ? "true" : "false");

    std::vector<BYTE> after;
    if (ReadMouseConfigRaw(&after)) {
        std::printf("写后 dpiSum=%d  dpi=", after[16]);
        for (int i = 0; i < 6; ++i) std::printf("%d ", after[4 + i * 2] | (after[5 + i * 2] << 8));
        std::printf(" 活动档=%d\n", after[2] & 0x0F);
    }
    return 0;
}
