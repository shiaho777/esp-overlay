package com.esp.core;

import android.util.Log;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;

/**
 * Memory reader that uses Shizuku privileged service instead of
 * direct process_vm_readv. This bypasses Android 11+ SELinux restrictions.
 */
public class ShizukuMemoryReader {
    private static final String TAG = "ESP-MemReader";

    private final ShizukuManager shizuku;
    private final int pid;

    public ShizukuMemoryReader(ShizukuManager shizuku, int pid) {
        this.shizuku = shizuku;
        this.pid = pid;
    }

    /// Read raw bytes from remote process.
    public byte[] readBytes(long address, int size) {
        if (shizuku == null || !shizuku.isBound()) return null;
        return shizuku.readMemory(pid, address, size);
    }

    /// Read a 32-bit integer.
    public int readInt(long address) {
        byte[] data = readBytes(address, 4);
        if (data == null || data.length < 4) return 0;
        return ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN).getInt();
    }

    /// Read a 64-bit long.
    public long readLong(long address) {
        byte[] data = readBytes(address, 8);
        if (data == null || data.length < 8) return 0;
        return ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN).getLong();
    }

    /// Read a 32-bit float.
    public float readFloat(long address) {
        byte[] data = readBytes(address, 4);
        if (data == null || data.length < 4) return 0;
        return ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN).getFloat();
    }

    /// Read a null-terminated string.
    public String readString(long address, int maxLen) {
        byte[] data = readBytes(address, maxLen);
        if (data == null) return "";
        int end = 0;
        while (end < data.length && data[end] != 0) end++;
        return new String(data, 0, end);
    }

    /// Read a 4x4 matrix (64 bytes, 16 floats).
    public float[] readMatrix4x4(long address) {
        byte[] data = readBytes(address, 64);
        if (data == null || data.length < 64) return null;
        float[] matrix = new float[16];
        ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN).asFloatBuffer().get(matrix);
        return matrix;
    }

    /// Read a Vec3 (12 bytes, 3 floats).
    public float[] readVec3(long address) {
        byte[] data = readBytes(address, 12);
        if (data == null || data.length < 12) return null;
        float[] vec = new float[3];
        ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN).asFloatBuffer().get(vec);
        return vec;
    }

    /// Get module base address.
    public long getModuleBase(String moduleName) {
        return shizuku.getModuleBase(pid, moduleName);
    }

    public int getPid() {
        return pid;
    }
}
