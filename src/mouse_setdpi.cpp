// mouse_setdpi.cpp - 直接把鼠标活动 DPI 档设为指定索引（用于恢复/排障）
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <cstdlib>

#include "device.h"

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    const int target = (argc > 1) ? std::atoi(argv[1]) : 1;

    const MouseConfig before = ReadMouseConfig();
    std::printf("before: dpiIndex=%d ok=%d\n", before.dpiIndex, before.ok ? 1 : 0);

    const bool ok = WriteMouseDpi(target);
    std::printf("WriteMouseDpi(%d) = %s\n", target, ok ? "true" : "false");

    const MouseConfig after = ReadMouseConfig();
    std::printf("after : dpiIndex=%d  DPI=%d\n", after.dpiIndex,
                (after.dpiIndex >= 0 && after.dpiIndex < 6) ? after.dpiList[after.dpiIndex] : -1);
    return ok ? 0 : 1;
}
