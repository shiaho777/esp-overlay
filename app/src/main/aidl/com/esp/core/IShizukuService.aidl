// IShizukuService.aidl
package com.esp.core;

interface IShizukuService {
    byte[] readMemory(int pid, long address, int size);
    int findPid(String packageName);
    long getModuleBase(int pid, String moduleName);

    // 诊断：返回 /proc/pid/maps 中所有含 .so 的行（每行一条）
    String[] listModules(int pid);

    // 诊断：返回所有匹配关键字的 maps 行
    String[] searchMaps(int pid, String keyword);

    // Native 内存扫描找模块基址（maps 不可读时使用）
    // 用 process_vm_readv 扫描进程内存找 ELF magic
    long findModuleBaseNative(int pid, String moduleName);

    // 诊断信息：测试对目标进程的各种访问能力
    String diagnosePid(int pid);
}
