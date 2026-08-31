#include "memory_reader.h"

#include <cstring>
#include <fstream>
#include <sstream>
#include <sys/uio.h>
#include <unistd.h>
#include <dirent.h>
#include <algorithm>
#include <unordered_map>

namespace esp {

MemoryReader::MemoryReader(pid_t pid) : pid_(pid), shizuku_mode_(false) {}

MemoryReader::MemoryReader(ReadCallback callback)
    : pid_(0), shizuku_mode_(true), callback_(std::move(callback)) {}

bool MemoryReader::read(uintptr_t remote_addr, void* buf, size_t len) const {
    if (len == 0) return true;

    if (shizuku_mode_) {
        if (!callback_) return false;
        size_t total = 0;
        while (total < len) {
            size_t n = callback_(remote_addr + total,
                                  reinterpret_cast<char*>(buf) + total,
                                  len - total);
            if (n == 0) return false;
            total += n;
            if (total >= len) break;
        }
        return total == len;
    }

    // Direct mode: process_vm_readv
    iovec local{buf, len};
    iovec remote{reinterpret_cast<void*>(remote_addr), len};

    size_t total = 0;
    while (total < len) {
        local.iov_base = reinterpret_cast<char*>(buf) + total;
        local.iov_len = len - total;
        remote.iov_base = reinterpret_cast<void*>(remote_addr + total);
        remote.iov_len = len - total;

        ssize_t n = process_vm_readv(pid_, &local, 1, &remote, 1, 0);
        if (n <= 0) {
            return false;
        }
        total += static_cast<size_t>(n);
    }
    return true;
}

std::string MemoryReader::read_string(uintptr_t addr, size_t max_len) const {
    std::vector<char> buf(max_len);
    if (!read(addr, buf.data(), max_len)) {
        // Try reading in smaller chunks in case the string spans a page boundary.
        for (size_t i = 0; i < max_len; ++i) {
            char c;
            if (!read(addr + i, &c, 1) || c == '\0') {
                return std::string(buf.data(), i);
            }
            buf[i] = c;
        }
        return std::string(buf.data(), max_len);
    }
    // Ensure null termination
    buf[max_len - 1] = '\0';
    return std::string(buf.data());
}

uintptr_t MemoryReader::get_module_base(const std::string& module_name) const {
    // Check cache first (Shizuku mode)
    {
        std::lock_guard<std::mutex> lock(module_cache_mutex_);
        auto it = module_cache_.find(module_name);
        if (it != module_cache_.end()) return it->second;
    }

    if (shizuku_mode_) {
        // In Shizuku mode, module base should be set externally
        return 0;
    }

    // Direct mode: read /proc/pid/maps
    std::string path = "/proc/" + std::to_string(pid_) + "/maps";
    std::ifstream f(path);
    if (!f.is_open()) return 0;

    std::string line;
    while (std::getline(f, line)) {
        if (line.find(module_name) == std::string::npos) continue;

        uintptr_t start = 0, end = 0;
        char perms[5] = {};
        int ret = sscanf(line.c_str(), "%lx-%lx %4s", &start, &end, perms);
        if (ret >= 3 && (perms[0] == 'r')) {
            return start;
        }
    }
    return 0;
}

void MemoryReader::set_module_base(const std::string& module_name, uintptr_t base) {
    std::lock_guard<std::mutex> lock(module_cache_mutex_);
    module_cache_[module_name] = base;
}

pid_t MemoryReader::find_pid(const std::string& package_name) {
    DIR* dir = opendir("/proc");
    if (!dir) return -1;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_type != DT_DIR && entry->d_type != DT_UNKNOWN) continue;
        char* endp;
        long pid = strtol(entry->d_name, &endp, 10);
        if (*endp != '\0' || pid <= 0) continue;

        if (pid == static_cast<long>(getpid())) continue;

        std::string cmdline_path = "/proc/" + std::string(entry->d_name) + "/cmdline";
        std::ifstream f(cmdline_path);
        if (!f.is_open()) continue;

        std::string cmdline;
        std::getline(f, cmdline, '\0');
        if (cmdline == package_name) {
            closedir(dir);
            return static_cast<pid_t>(pid);
        }
    }
    closedir(dir);
    return -1;
}

} // namespace esp
