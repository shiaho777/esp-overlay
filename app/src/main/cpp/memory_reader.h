#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <functional>
#include <mutex>
#include <unordered_map>

namespace esp {

/// Memory reader callback type.
/// Takes (remote_addr, buf, len) → returns bytes_read (0 on failure).
using ReadCallback = std::function<size_t(uintptr_t remote_addr, void* buf, size_t len)>;

/// Remote memory reader.
/// 
/// Two modes:
/// 1. Direct mode: uses process_vm_readv (requires root or shell UID)
/// 2. Shizuku mode: calls back through JNI to Java ShizukuMemoryReader
///    (works without root by using Shizuku's shell-UID process)
class MemoryReader {
public:
    /// Direct mode constructor (process_vm_readv)
    explicit MemoryReader(pid_t pid);

    /// Shizuku callback mode constructor
    explicit MemoryReader(ReadCallback callback);

    /// Read `len` bytes from `remote_addr` into `buf`.
    /// Returns true on success.
    bool read(uintptr_t remote_addr, void* buf, size_t len) const;

    /// Typed read convenience.
    template <typename T>
    bool read_t(uintptr_t addr, T& out) const {
        return read(addr, &out, sizeof(T));
    }

    /// Read a null-terminated string (max 4096 bytes).
    std::string read_string(uintptr_t addr, size_t max_len = 4096) const;

    /// Read a vector of N elements.
    template <typename T>
    std::vector<T> read_array(uintptr_t addr, size_t count) const {
        std::vector<T> result(count);
        if (!read(addr, result.data(), count * sizeof(T))) {
            return {};
        }
        return result;
    }

    /// Find the base address of a loaded library in the target process.
    /// Returns 0 if not found.
    /// NOTE: In Shizuku mode, this is set externally by Java code.
    uintptr_t get_module_base(const std::string& module_name) const;

    /// Set module base address (used in Shizuku mode where Java resolves it)
    void set_module_base(const std::string& module_name, uintptr_t base);

    /// Get the PID of a process by package name (direct mode only).
    static pid_t find_pid(const std::string& package_name);

    pid_t pid() const { return pid_; }

    bool is_shizuku_mode() const { return shizuku_mode_; }

private:
    pid_t pid_ = 0;
    bool shizuku_mode_ = false;
    ReadCallback callback_;

    /// Cached module base addresses (for Shizuku mode)
    mutable std::mutex module_cache_mutex_;
    std::unordered_map<std::string, uintptr_t> module_cache_;
};

} // namespace esp
