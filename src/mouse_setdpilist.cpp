// mouse_setdpilist.cpp - 直接写入一组 DPI 档位
// 用法：mouse_setdpilist.exe 400 800 3200 26000
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

#include "device.h"

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    int vals[6] = {};
    int n = 0;
    for (int i = 1; i < argc && n < 6; ++i) {
        const int v = std::atoi(argv[i]);
        if (v >= 100 && v <= 26000) vals[n++] = v;
    }
    if (n < 1) { std::printf("用法: mouse_setdpilist.exe 400 800 3200 26000\n"); return 1; }

    const int active = (n >= 2) ? 1 : 0;
    std::printf("写入 %d 档（活动档 %d = %d）…\n", n, active, vals[active]);
    const bool ok = SetMouseDpiStages(vals, n, active);
    std::printf("  结果: %s\n", ok ? "true" : "false");

    std::vector<BYTE> after;
    if (ReadMouseConfigRaw(&after)) {
        std::printf("当前 dpiSum=%d  dpi=", after[16]);
        for (int i = 0; i < 6; ++i) std::printf("%d ", after[4 + i * 2] | (after[5 + i * 2] << 8));
        std::printf(" 活动档=%d\n", after[2] & 0x0F);
    }
    return 0;
}
