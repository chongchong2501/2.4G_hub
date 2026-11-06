// mouse_nibble_probe.cpp - 判定 freeRate（2.4G 回报率）落在哪个 nibble
//
// 做法：读原配置 → 用单命令 0x11 0x41 把 freeRate 写成 2 → 读回看哪个 nibble 变
//       → 立即写回原值 → 再读确认整包一致。
// 前置：device 层已强制"写前必读"，通道不通则不会写。

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

#include "device.h"

namespace {
void DumpNibbles(const char* tag, const std::vector<BYTE>& p) {
    std::printf("%s b1=0x%02X (低%u 高%u)  b2=0x%02X (低%u 高%u)\n", tag,
                p[1], p[1] & 0xF, p[1] >> 4, p[2], p[2] & 0xF, p[2] >> 4);
}
}  // namespace

int main() {
    SetConsoleOutputCP(CP_UTF8);

    std::vector<BYTE> orig;
    if (!ReadMouseConfigRaw(&orig)) {
        std::printf("✗ 读取失败，通道不可用，终止（不会写任何东西）\n");
        return 1;
    }
    DumpNibbles("初始:", orig);

    const int usbGuess = orig[1] & 0x0F;
    std::printf("\n→ 写 0x11 0x41: usbRate=%d freeRate=2 …\n", usbGuess);
    if (!WriteMouseRate(usbGuess, 2)) {
        std::printf("✗ 写入被拒（通道问题），终止\n");
        return 1;
    }
    Sleep(400);

    std::vector<BYTE> after;
    if (ReadMouseConfigRaw(&after)) {
        DumpNibbles("写后:", after);
        if (after[1] != orig[1]) std::printf("  · b1 改变: 0x%02X → 0x%02X\n", orig[1], after[1]);
        if (after[2] != orig[2]) std::printf("  · b2 改变: 0x%02X → 0x%02X\n", orig[2], after[2]);

        if (after[2] == ((orig[2] & 0xF0) | 2) && after[1] == orig[1]) {
            std::printf("  ⇒ freeRate 位于 b2 低 nibble → 文档读序成立（gRate = b2 低）\n");
        } else if (after[1] == ((orig[1] & 0xF0) | 2) && after[2] == orig[2]) {
            std::printf("  ⇒ freeRate 位于 b1 低 nibble → 与文档读序相反\n");
        } else if (after[1] == orig[1] && after[2] == orig[2]) {
            std::printf("  ⇒ 两个字节都没变，写入未生效\n");
        } else {
            std::printf("  ⇒ 变化不符合单一 nibble 假设，需人工判断\n");
        }
    } else {
        std::printf("✗ 写后读取失败\n");
    }

    std::printf("\n→ 恢复: usbRate=%u freeRate=%u …\n", orig[1] & 0xF, orig[2] & 0xF);
    if (!WriteMouseRate(orig[1] & 0xF, orig[2] & 0xF)) {
        std::printf("✗ 恢复写入失败，请用备份还原\n");
        return 1;
    }
    Sleep(400);

    std::vector<BYTE> finalState;
    if (ReadMouseConfigRaw(&finalState)) {
        DumpNibbles("恢复后:", finalState);
        const bool same = finalState[1] == orig[1] && finalState[2] == orig[2];
        std::printf("  %s\n", same ? "✓ 已回到初始值" : "✗ 与初始不一致，请用备份还原");
        return same ? 0 : 2;
    }
    return 1;
}
