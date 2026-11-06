// mouse_dpiswitch_test.cpp - 单独验证 WriteMouseDpi（隔离 UI 消息问题）
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <vector>

#include "device.h"

int main() {
    SetConsoleOutputCP(CP_UTF8);

    std::vector<BYTE> before;
    if (!ReadMouseConfigRaw(&before)) { std::printf("✗ 初始读取失败\n"); return 1; }
    const int origDpi = before[2] & 0x0F;
    std::printf("初始: b2=0x%02X  活动 DPI 档=%d\n", before[2], origDpi);

    const int target = (origDpi == 2) ? 1 : 2;
    std::printf("\n→ 切到档 %d …\n", target);
    const bool ok = WriteMouseDpi(target);
    std::printf("  WriteMouseDpi 返回: %s\n", ok ? "true" : "false");

    std::vector<BYTE> after;
    if (ReadMouseConfigRaw(&after)) {
        std::printf("  切换后: b2=0x%02X  活动 DPI 档=%d\n", after[2], after[2] & 0x0F);
    }

    std::printf("\n→ 切回档 %d …\n", origDpi);
    const bool back = WriteMouseDpi(origDpi);
    std::printf("  返回: %s\n", back ? "true" : "false");

    std::vector<BYTE> finalState;
    if (ReadMouseConfigRaw(&finalState)) {
        std::printf("  最终: b2=0x%02X  活动 DPI 档=%d %s\n", finalState[2], finalState[2] & 0x0F,
                    (finalState[2] & 0x0F) == origDpi ? "✓ 已复原" : "✗ 未复原");
    }
    return 0;
}
