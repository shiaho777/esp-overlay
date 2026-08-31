package com.esp.core;

/**
 * Native bridge to the ESP engine (C++ via JNI).
 *
 * Two init modes:
 * 1. Direct mode: init(packageName) — uses process_vm_readv directly (needs root)
 * 2. Shizuku mode: initWithShizuku(pid, il2cppBase, gamecoreBase) —
 *    reads memory via JNI callback to readCallback() (no root needed)
 */
public class EspNative {
    static {
        System.loadLibrary("esp-native");
    }

    /// Direct mode init (requires root/shell UID)
    public static native boolean init(String packageName);

    /// Shizuku mode init — memory reads go through Java callback
    /// pid: game process PID (from Shizuku findPid)
    /// il2cppBase: base address of libil2cpp.so (from Shizuku getModuleBase)
    /// gamecoreBase: base address of libGameCore.so (0 if not loaded)
    public static native boolean initWithShizuku(int pid, long il2cppBase, long gamecoreBase);

    /// Memory read callback — called from C++ via JNI.
    /// Delegates to the ShizukuMemoryReader to read remote process memory.
    /// Returns byte array of read data, or null on failure.
    private static ShizukuMemoryReader sReader = null;

    public static void setMemoryReader(ShizukuMemoryReader reader) {
        sReader = reader;
    }

    /// Called from C++ via JNI (FindStaticMethod)
    public static byte[] readCallback(int pid, long address, int size) {
        if (sReader == null) return null;
        if (sReader.getPid() != pid) return null;
        return sReader.readBytes(address, size);
    }

    public static native String getLastError();
    public static native void start();
    public static native void stop();
    public static native void setScreenSize(int width, int height);
    public static native Entity[] getEntities();
    public static native void setConfig(
        boolean showEnemies, boolean showAllies,
        boolean showMinions, boolean showJungle,
        boolean showHp, boolean showNames, boolean showDistance,
        boolean showBoxes, boolean showLines);
    public static native boolean isRunning();
}
