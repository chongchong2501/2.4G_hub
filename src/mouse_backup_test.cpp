// mouse_backup_test.cpp - 验证「备份 → 写入 → 回读 → 恢复」全链路
//
// 只改 sleep 字段（分钟），改动可逆且用户无感；结束时从备份还原并校验。
//
// 编译：cl /std:c++17 /utf-8 /EHsc /MT /O2 mouse_backup_test.cpp device.cpp

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

#include "device.h"

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);

    std::wstring dir = L"E:\\program\\2.4G_hub\\bin\\config";
    if (argc > 1) {
        int n = MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, nullptr, 0);
        dir.assign(static_cast<size_t>(n - 1), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, dir.data(), n);
    }

    std::printf("配置目录: %ls\n\n", dir.c_str());

    std::wstring backup;
    if (!EnsureMouseBackup(dir, &backup)) {
        std::printf("✗ 备份失败（设备未连接或通道卡死）\n");
        return 1;
    }
    std::printf("✓ 备份就绪: %ls\n", backup.c_str());

    std::vector<BYTE> original;
    if (!ReadMouseConfigRaw(&original)) {
        std::printf("✗ 读取原配置失败\n");
        return 1;
    }
    const BYTE oldSleep = original[19];
    std::printf("  当前 sleep = %u min，profile = %u\n\n", oldSleep, original[0]);

    // 改一个无害字段并写回
    std::vector<BYTE> modified = original;
    modified[19] = 60;
    std::printf("→ 写入 sleep = 60 …\n");
    if (!WriteMouseConfigRaw(modified)) {
        std::printf("✗ 写入失败（可能是通道问题或校验不一致）\n");
        return 1;
    }

    std::vector<BYTE> afterWrite;
    if (ReadMouseConfigRaw(&afterWrite)) {
        std::printf("  写入后 sleep = %u（期望 60）%s\n\n", afterWrite[19],
                    afterWrite[19] == 60 ? "✓" : "✗");
    }

    // 从备份恢复
    std::printf("→ 从备份恢复 …\n");
    if (!RestoreMouseConfig(dir)) {
        std::printf("✗ 恢复失败\n");
        return 1;
    }

    std::vector<BYTE> finalState;
    if (!ReadMouseConfigRaw(&finalState)) {
        std::printf("✗ 恢复后读取失败\n");
        return 1;
    }

    const bool same = finalState[19] == oldSleep;
    std::printf("  恢复后 sleep = %u（原值 %u）%s\n", finalState[19], oldSleep, same ? "✓" : "✗");

    // 整包逐字节比对（忽略硬件自维护的 offset 3 与 sensor bit0）
    int diff = 0;
    for (size_t i = 0; i < original.size() && i < finalState.size(); ++i) {
        if (i == 3) continue;
        BYTE a = original[i];
        BYTE b = finalState[i];
        if (i == 17) { a &= 0xFE; b &= 0xFE; }
        if (a != b) { ++diff; std::printf("    差异 +%02zX: %02X → %02X\n", i, a, b); }
    }
    std::printf("\n整包比对差异字节数: %d %s\n", diff, diff == 0 ? "✓ 完全一致" : "✗ 存在残留差异");
    return diff == 0 ? 0 : 2;
}
