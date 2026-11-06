// mouse_restore.cpp - 从 config\backup.mouse.bin 整包还原，并校验
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

    std::printf("备份文件: %ls\n", MouseBackupPath(dir).c_str());

    std::vector<BYTE> before;
    if (ReadMouseConfigRaw(&before)) {
        std::printf("还原前: b1=0x%02X  b2=0x%02X\n", before[1], before[2]);
    }

    if (!RestoreMouseConfig(dir)) {
        std::printf("✗ 还原失败\n");
        return 1;
    }
    Sleep(300);

    std::vector<BYTE> after;
    if (!ReadMouseConfigRaw(&after)) {
        std::printf("✗ 还原后读取失败\n");
        return 1;
    }
    std::printf("还原后: b1=0x%02X  b2=0x%02X  sleep=%u  profile=%u\n",
                after[1], after[2], after[19], after[0]);

    // 与备份文件逐字节比对
    HANDLE f = CreateFileW(MouseBackupPath(dir).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return 1;
    DWORD size = GetFileSize(f, nullptr);
    std::vector<BYTE> backup(size);
    DWORD read = 0;
    ReadFile(f, backup.data(), size, &read, nullptr);
    CloseHandle(f);

    int diff = 0;
    for (size_t i = 0; i < backup.size() && i < after.size(); ++i) {
        if (i == 3) continue;
        BYTE a = backup[i];
        BYTE b = after[i];
        if (i == 17) { a &= 0xFE; b &= 0xFE; }
        if (a != b) {
            ++diff;
            std::printf("  差异 +%02zX: 备份 %02X / 当前 %02X\n", i, a, b);
        }
    }
    std::printf("\n与备份差异字节: %d %s\n", diff, diff == 0 ? "✓ 完全还原" : "✗ 未完全还原");
    return diff == 0 ? 0 : 2;
}
