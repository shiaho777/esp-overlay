package com.esp.core;

import android.os.RemoteException;
import android.util.Log;
import androidx.annotation.Keep;

import java.io.BufferedReader;
import java.io.FileReader;
import java.io.File;
import java.util.ArrayList;
import java.util.List;

/**
 * Shizuku UserService — runs in a privileged process (shell UID).
 * Has permission to call process_vm_readv on other processes.
 *
 * Shizuku requires a no-arg constructor.
 */
@Keep
public class ShizukuService extends IShizukuService.Stub {

    private static final String TAG = "ESP-ShizukuService";

    /// No-arg constructor required by Shizuku
    public ShizukuService() {
        Log.i(TAG, "ShizukuService created in privileged process");
        // Shizuku UserService runs in a SEPARATE process — must load native lib here
        try {
            System.loadLibrary("esp-native");
            Log.i(TAG, "esp-native loaded in Shizuku process");
        } catch (UnsatisfiedLinkError e) {
            Log.e(TAG, "Failed to load esp-native in Shizuku process", e);
        }
    }

    @Override
    public byte[] readMemory(int pid, long address, int size) throws RemoteException {
        if (size <= 0 || size > 1024 * 1024) return null;
        return nativeReadMemory(pid, address, size);
    }

    @Override
    public int findPid(String packageName) throws RemoteException {
        Log.i(TAG, "findPid: looking for " + packageName);
        try {
            File procDir = new File("/proc");
            File[] files = procDir.listFiles();
            if (files == null) {
                Log.e(TAG, "findPid: /proc listFiles returned null");
                return -1;
            }

            // Collect all matching PIDs
            List<Integer> matchingPids = new ArrayList<>();
            for (File f : files) {
                String name = f.getName();
                int pid;
                try {
                    pid = Integer.parseInt(name);
                } catch (NumberFormatException e) {
                    continue;
                }

                try (BufferedReader reader = new BufferedReader(
                        new FileReader(new File(f, "cmdline")))) {
                    String cmdline = reader.readLine();
                    if (cmdline != null) {
                        String trimmed = cmdline.trim();
                        int nullIdx = trimmed.indexOf('\0');
                        if (nullIdx >= 0) trimmed = trimmed.substring(0, nullIdx);
                        if (trimmed.equals(packageName)) {
                            Log.i(TAG, "findPid: found " + packageName + " pid=" + pid);
                            matchingPids.add(pid);
                        }
                    }
                } catch (Exception e) {
                    // Can't read this process's cmdline
                }
            }

            if (matchingPids.isEmpty()) {
                Log.e(TAG, "findPid: " + packageName + " not found in /proc");
                return -1;
            }

            // If only one process, return it
            if (matchingPids.size() == 1) {
                return matchingPids.get(0);
            }

            // Multiple processes — find the one that has libil2cpp.so in maps
            Log.i(TAG, "findPid: found " + matchingPids.size() + " processes, checking for il2cpp...");
            for (int pid : matchingPids) {
                if (hasModuleInMaps(pid, "libil2cpp.so")) {
                    Log.i(TAG, "findPid: pid=" + pid + " has libil2cpp.so, using this one");
                    return pid;
                }
            }

            // Fallback: return the first one
            Log.w(TAG, "findPid: no process has libil2cpp.so, returning first: " + matchingPids.get(0));
            return matchingPids.get(0);
        } catch (Exception e) {
            Log.e(TAG, "findPid error", e);
            return -1;
        }
    }

    /// Check if a process has a specific module loaded in its /proc/pid/maps
    private boolean hasModuleInMaps(int pid, String moduleName) {
        try (BufferedReader reader = new BufferedReader(
                new FileReader(new File("/proc/" + pid + "/maps")))) {
            String line;
            while ((line = reader.readLine()) != null) {
                if (line.contains(moduleName)) return true;
            }
        } catch (Exception e) {
            // Can't read maps
        }
        return false;
    }

    @Override
    public long getModuleBase(int pid, String moduleName) throws RemoteException {
        Log.i(TAG, "getModuleBase: pid=" + pid + " module=" + moduleName);
        try {
            File mapsFile = new File("/proc/" + pid + "/maps");
            if (!mapsFile.exists()) {
                Log.e(TAG, "getModuleBase: /proc/" + pid + "/maps does not exist");
                return 0;
            }

            // 检查 maps 文件是否可读且有内容
            long fileSize = mapsFile.length();
            Log.i(TAG, "getModuleBase: maps file size=" + fileSize);
            if (fileSize == 0) {
                Log.e(TAG, "getModuleBase: /proc/" + pid + "/maps is empty or unreadable (permission denied?)");
                return 0;
            }

            long baseAddr = 0;
            List<String> allModules = new ArrayList<>();
            int totalLines = 0;
            int readableSoLines = 0;

            try (BufferedReader reader = new BufferedReader(new FileReader(mapsFile))) {
                String line;
                while ((line = reader.readLine()) != null) {
                    totalLines++;
                    String trimmed = line.trim();
                    if (trimmed.isEmpty()) continue;

                    // Format: address perms offset dev inode pathname
                    String[] parts = trimmed.split("\\s+", 6);
                    if (parts.length < 6) continue;

                    String addrRange = parts[0];
                    String perms = parts[1];
                    String pathname = parts[5];

                    // Track unique .so files
                    if (pathname.endsWith(".so")) {
                        String soName = pathname.substring(pathname.lastIndexOf('/') + 1);
                        if (!allModules.contains(soName)) {
                            allModules.add(soName);
                        }
                        // 统计可读的 .so 段
                        if (perms.startsWith("r")) {
                            readableSoLines++;
                        }
                    }

                    // 精确匹配：pathname 以 /moduleName 结尾，或等于 moduleName
                    // 也支持 contains 匹配（处理路径前缀不同的情况）
                    boolean match = false;
                    if (pathname.endsWith("/" + moduleName) || pathname.equals(moduleName)) {
                        match = true;
                    } else if (pathname.contains(moduleName)) {
                        // 模糊匹配作为 fallback，但记录日志
                        match = true;
                    }

                    if (match && baseAddr == 0) {
                        // 只接受第一个可读段作为基址
                        if (!perms.startsWith("r")) continue;
                        String[] range = addrRange.split("-");
                        if (range.length < 2) continue;
                        try {
                            baseAddr = Long.parseLong(range[0], 16);
                            Log.i(TAG, "getModuleBase: found " + moduleName +
                                  " at 0x" + Long.toHexString(baseAddr) +
                                  " perms=" + perms + " path=" + pathname);
                        } catch (NumberFormatException e) {
                            // skip
                        }
                    }
                }
            }

            Log.i(TAG, "getModuleBase: scanned " + totalLines + " lines, " +
                  allModules.size() + " unique .so, " + readableSoLines + " readable .so segments");

            if (baseAddr == 0) {
                Log.e(TAG, "getModuleBase: " + moduleName + " not found in maps for pid " + pid);

                // 打印所有 .so 文件名
                Log.i(TAG, "getModuleBase: all loaded .so files:");
                for (String mod : allModules) {
                    Log.i(TAG, "  .so: " + mod);
                }

                // 尝试部分匹配
                String shortName = moduleName;
                if (shortName.startsWith("lib")) shortName = shortName.substring(3);
                String shortLower = shortName.toLowerCase();
                for (String mod : allModules) {
                    String modLower = mod.toLowerCase();
                    if (modLower.contains(shortLower) || shortLower.contains(modLower)) {
                        Log.i(TAG, "  possible match: " + mod);
                    }
                }
            }

            return baseAddr;
        } catch (java.io.FileNotFoundException e) {
            Log.e(TAG, "getModuleBase: cannot read /proc/" + pid + "/maps — permission denied (shell UID can't read target maps?)", e);
            return 0;
        } catch (Exception e) {
            Log.e(TAG, "getModuleBase error for pid=" + pid + " module=" + moduleName, e);
            return 0;
        }
    }

    @Override
    public String[] listModules(int pid) throws RemoteException {
        Log.i(TAG, "listModules: pid=" + pid);
        List<String> result = new ArrayList<>();
        try {
            File mapsFile = new File("/proc/" + pid + "/maps");
            if (!mapsFile.exists()) {
                result.add("ERROR: /proc/" + pid + "/maps does not exist");
                return result.toArray(new String[0]);
            }

            try (BufferedReader reader = new BufferedReader(new FileReader(mapsFile))) {
                String line;
                while ((line = reader.readLine()) != null) {
                    String trimmed = line.trim();
                    if (trimmed.isEmpty()) continue;
                    String[] parts = trimmed.split("\\s+", 6);
                    if (parts.length < 6) continue;
                    String pathname = parts[5];
                    if (pathname.endsWith(".so")) {
                        result.add(line);
                    }
                }
            }
        } catch (Exception e) {
            result.add("ERROR: " + e.getMessage());
        }
        Log.i(TAG, "listModules: found " + result.size() + " .so entries");
        return result.toArray(new String[0]);
    }

    @Override
    public String[] searchMaps(int pid, String keyword) throws RemoteException {
        Log.i(TAG, "searchMaps: pid=" + pid + " keyword=" + keyword);
        List<String> result = new ArrayList<>();
        try {
            File mapsFile = new File("/proc/" + pid + "/maps");
            if (!mapsFile.exists()) {
                result.add("ERROR: /proc/" + pid + "/maps does not exist");
                return result.toArray(new String[0]);
            }

            try (BufferedReader reader = new BufferedReader(new FileReader(mapsFile))) {
                String line;
                while ((line = reader.readLine()) != null) {
                    if (line.contains(keyword)) {
                        result.add(line);
                    }
                }
            }
        } catch (Exception e) {
            result.add("ERROR: " + e.getMessage());
        }
        Log.i(TAG, "searchMaps: found " + result.size() + " matches");
        return result.toArray(new String[0]);
    }

    @Override
    public long findModuleBaseNative(int pid, String moduleName) throws RemoteException {
        Log.i(TAG, "findModuleBaseNative: pid=" + pid + " module=" + moduleName);
        try {
            return nativeFindModuleBase(pid, moduleName);
        } catch (UnsatisfiedLinkError e) {
            Log.e(TAG, "findModuleBaseNative: native lib not loaded", e);
            return -2;  // special error code: native not loaded
        }
    }

    @Override
    public String diagnosePid(int pid) throws RemoteException {
        Log.i(TAG, "diagnosePid: pid=" + pid);
        try {
            String result = nativeDiagnosePid(pid);
            if (result == null) {
                return "ERROR: nativeDiagnosePid returned null";
            }
            return result;
        } catch (UnsatisfiedLinkError e) {
            Log.e(TAG, "diagnosePid: native lib not loaded", e);
            return "ERROR: native lib not loaded in Shizuku process: " + e.getMessage();
        }
    }

    /// Native: process_vm_readv — runs with shell privileges via Shizuku
    @Keep
    private static native byte[] nativeReadMemory(int pid, long address, int size);

    /// Native: find module base via memory scan (fallback when maps unreadable)
    @Keep
    private static native long nativeFindModuleBase(int pid, String moduleName);

    /// Native: diagnostic info for a pid
    @Keep
    private static native String nativeDiagnosePid(int pid);
}
