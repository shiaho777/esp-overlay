#include <jni.h>
#include <android/log.h>
#include <errno.h>
#include <string.h>
#include <string>
#include <vector>
#include <memory>
#include <atomic>
#include <mutex>
#include <sys/uio.h>
#include <unistd.h>
#include <fcntl.h>
#include <elf.h>
#include <sys/auxv.h>
#include "esp_engine.h"
#include "memory_reader.h"

#define LOG_TAG "ESP"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static std::unique_ptr<esp::EspEngine> g_engine;
static std::string g_last_error;

// --- Shizuku callback mechanism ---
// C++ calls back into Java EspNative.nativeReadCallback(pid, addr, size)
// which delegates to ShizukuMemoryReader → Shizuku service → process_vm_readv

static JavaVM* g_jvm = nullptr;
static jobject g_global_ref = nullptr;  // Global ref to EspNative class
static std::atomic<int> g_shizuku_pid{0};
static std::mutex g_callback_mutex;

/// JNI callback: read remote memory via Java Shizuku service
static size_t shizuku_read_callback(uintptr_t remote_addr, void* buf, size_t len) {
    if (!g_jvm || !g_global_ref) return 0;

    JNIEnv* env = nullptr;
    bool attached = false;
    JavaVMAttachArgs args = {JNI_VERSION_1_6, "ESP-Callback", nullptr};
    jint ret = g_jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (ret != JNI_OK) {
        if (g_jvm->AttachCurrentThread(&env, &args) != JNI_OK) {
            return 0;
        }
        attached = true;
    }

    size_t bytes_read = 0;

    // Cap the read size to avoid huge allocations
    const size_t MAX_CHUNK = 65536;
    size_t remaining = len;
    char* dst = static_cast<char*>(buf);

    while (remaining > 0) {
        size_t chunk = (remaining > MAX_CHUNK) ? MAX_CHUNK : remaining;

        std::lock_guard<std::mutex> lock(g_callback_mutex);
        jclass cls = env->GetObjectClass(g_global_ref);
        if (!cls) break;

        jmethodID mid = env->GetStaticMethodID(cls, "readCallback",
            "(IJI)[B");
        if (!mid) {
            env->DeleteLocalRef(cls);
            break;
        }

        jbyteArray result = static_cast<jbyteArray>(
            env->CallStaticObjectMethod(cls, mid,
                static_cast<jint>(g_shizuku_pid.load()),
                static_cast<jlong>(remote_addr + bytes_read),
                static_cast<jint>(chunk)));

        env->DeleteLocalRef(cls);

        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            break;
        }

        if (!result) break;

        jsize actual_len = env->GetArrayLength(result);
        if (actual_len <= 0) {
            env->DeleteLocalRef(result);
            break;
        }

        env->GetByteArrayRegion(result, 0, actual_len,
            reinterpret_cast<jbyte*>(dst + bytes_read));
        env->DeleteLocalRef(result);

        bytes_read += static_cast<size_t>(actual_len);
        remaining -= static_cast<size_t>(actual_len);

        // If we got less than requested, stop (can't read further)
        if (static_cast<size_t>(actual_len) < chunk) break;
    }

    if (attached) {
        g_jvm->DetachCurrentThread();
    }

    return bytes_read;
}

extern "C" {

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
    g_jvm = vm;
    LOGI("JNI_OnLoad: JavaVM saved");
    return JNI_VERSION_1_6;
}

// --- Direct mode (requires root/shell UID) ---

JNIEXPORT jboolean JNICALL
Java_com_esp_core_EspNative_init(JNIEnv* env, jclass cls, jstring package_name) {
    const char* pkg = env->GetStringUTFChars(package_name, nullptr);
    std::string pkg_str(pkg);
    env->ReleaseStringUTFChars(package_name, pkg);

    LOGI("=== ESP Direct Init for %s ===", pkg_str.c_str());

    // Find PID
    pid_t pid = esp::MemoryReader::find_pid(pkg_str);
    LOGI("find_pid('%s') = %d", pkg_str.c_str(), pid);
    if (pid <= 0) {
        g_last_error = "找不到游戏进程 (pid=" + std::to_string(pid) + ")";
        return JNI_FALSE;
    }

    auto reader = std::make_unique<esp::MemoryReader>(pid);

    // Find libil2cpp.so base
    uintptr_t base = reader->get_module_base("libil2cpp.so");
    LOGI("libil2cpp.so base = 0x%lx", base);
    if (!base) {
        g_last_error = "无法读取游戏内存映射，可能需要root或Shizuku";
        return JNI_FALSE;
    }

    // Test read
    uint8_t test_byte = 0;
    if (!reader->read(base, &test_byte, 1)) {
        g_last_error = "process_vm_readv被拒绝 (errno=" + std::to_string(errno) + ")，请使用Shizuku模式";
        return JNI_FALSE;
    }

    g_engine = std::make_unique<esp::EspEngine>();
    if (g_engine->init(std::move(reader), base, reader->get_module_base("libGameCore.so"))) {
        LOGI("ESP engine initialized (direct mode)");
        return JNI_TRUE;
    } else {
        g_last_error = "引擎初始化失败";
        g_engine.reset();
        return JNI_FALSE;
    }
}

// --- Shizuku mode (no root needed) ---

JNIEXPORT jboolean JNICALL
Java_com_esp_core_EspNative_initWithShizuku(JNIEnv* env, jclass cls,
        jint pid, jlong il2cpp_base, jlong gamecore_base) {
    LOGI("=== ESP Shizuku Init: pid=%d, il2cpp=0x%llx, gamecore=0x%llx ===",
         pid, (long long)il2cpp_base, (long long)gamecore_base);

    g_last_error.clear();

    if (pid <= 0) {
        g_last_error = "无效的PID";
        return JNI_FALSE;
    }
    if (il2cpp_base == 0) {
        g_last_error = "libil2cpp.so基址为0";
        return JNI_FALSE;
    }

    // Save global ref to EspNative class for callback
    if (!g_global_ref) {
        g_global_ref = env->NewGlobalRef(cls);
        if (!g_global_ref) {
            g_last_error = "无法创建全局引用";
            return JNI_FALSE;
        }
    }

    g_shizuku_pid.store(pid);

    // Create Shizuku-backed memory reader
    auto reader = std::make_unique<esp::MemoryReader>(shizuku_read_callback);
    reader->set_module_base("libil2cpp.so", static_cast<uintptr_t>(il2cpp_base));
    if (gamecore_base != 0) {
        reader->set_module_base("libGameCore.so", static_cast<uintptr_t>(gamecore_base));
    }

    // Test read
    uint8_t test_byte = 0;
    bool can_read = reader->read(static_cast<uintptr_t>(il2cpp_base), &test_byte, 1);
    LOGI("Shizuku read test: %s (byte=0x%02x)", can_read ? "OK" : "FAILED", test_byte);
    if (!can_read) {
        g_last_error = "Shizuku内存读取失败，请检查Shizuku服务是否正常运行";
        return JNI_FALSE;
    }

    // Create and init engine
    g_engine = std::make_unique<esp::EspEngine>();
    if (g_engine->init(std::move(reader),
                       static_cast<uintptr_t>(il2cpp_base),
                       static_cast<uintptr_t>(gamecore_base))) {
        LOGI("ESP engine initialized (Shizuku mode)");
        return JNI_TRUE;
    } else {
        if (g_last_error.empty()) g_last_error = "引擎初始化失败 (il2cpp元数据解析失败)";
        LOGE("%s", g_last_error.c_str());
        g_engine.reset();
        return JNI_FALSE;
    }
}

// --- Java callback entry point ---
// C++ calls Java EspNative.readCallback(pid, addr, size) via JNI.
// Java side delegates to ShizukuMemoryReader → Shizuku service → process_vm_readv.

JNIEXPORT jstring JNICALL
Java_com_esp_core_EspNative_getLastError(JNIEnv* env, jclass cls) {
    return env->NewStringUTF(g_last_error.c_str());
}

JNIEXPORT void JNICALL
Java_com_esp_core_EspNative_start(JNIEnv* env, jclass cls) {
    if (g_engine) {
        g_engine->start();
        LOGI("ESP engine started");
    }
}

JNIEXPORT void JNICALL
Java_com_esp_core_EspNative_stop(JNIEnv* env, jclass cls) {
    if (g_engine) {
        g_engine->stop();
        LOGI("ESP engine stopped");
    }
}

JNIEXPORT void JNICALL
Java_com_esp_core_EspNative_setScreenSize(JNIEnv* env, jclass cls, jint width, jint height) {
    LOGI("Screen size: %dx%d", width, height);
    if (g_engine) {
        g_engine->set_screen_size(width, height);
    }
}

JNIEXPORT jobjectArray JNICALL
Java_com_esp_core_EspNative_getEntities(JNIEnv* env, jclass cls) {
    if (!g_engine || !g_engine->is_running()) {
        return env->NewObjectArray(0, env->FindClass("com/esp/core/Entity"), nullptr);
    }

    auto entities = g_engine->get_entities();

    jclass entity_class = env->FindClass("com/esp/core/Entity");
    jmethodID ctor = env->GetMethodID(entity_class, "<init>", "(FFFFIIZ)V");

    jobjectArray result = env->NewObjectArray(entities.size(), entity_class, nullptr);
    for (size_t i = 0; i < entities.size(); i++) {
        const auto& e = entities[i];
        jobject obj = env->NewObject(entity_class, ctor,
            e.screen.x, e.screen.y,
            e.position.x, e.position.z,
            static_cast<jint>(e.actor_type),
            static_cast<jint>(e.camp),
            e.screen.visible ? JNI_TRUE : JNI_FALSE
        );
        env->SetObjectArrayElement(result, i, obj);
        env->DeleteLocalRef(obj);
    }
    return result;
}

JNIEXPORT void JNICALL
Java_com_esp_core_EspNative_setConfig(JNIEnv* env, jclass cls,
    jboolean showEnemies, jboolean showAllies,
    jboolean showMinions, jboolean showJungle,
    jboolean showHp, jboolean showNames, jboolean showDistance,
    jboolean showBoxes, jboolean showLines) {
    if (!g_engine) return;
    esp::EspConfig cfg;
    cfg.show_enemies = showEnemies;
    cfg.show_allies = showAllies;
    cfg.show_minions = showMinions;
    cfg.show_jungle = showJungle;
    cfg.show_hp_bars = showHp;
    cfg.show_names = showNames;
    cfg.show_distance = showDistance;
    cfg.show_boxes = showBoxes;
    cfg.show_lines = showLines;
    g_engine->set_config(cfg);
}

JNIEXPORT jboolean JNICALL
Java_com_esp_core_EspNative_isRunning(JNIEnv* env, jclass cls) {
    return (g_engine && g_engine->is_running()) ? JNI_TRUE : JNI_FALSE;
}

/// JNI method for ShizukuService.nativeReadMemory — runs in Shizuku's privileged process
JNIEXPORT jbyteArray JNICALL
Java_com_esp_core_ShizukuService_nativeReadMemory(JNIEnv* env, jclass cls,
    jint pid, jlong address, jint size) {
    if (size <= 0 || size > 1024 * 1024) return nullptr;

    jbyteArray result = env->NewByteArray(size);
    if (!result) return nullptr;

    // Use process_vm_readv — this runs with shell UID via Shizuku, so it has permission
    iovec local;
    iovec remote;

    std::vector<uint8_t> buf(size);
    local.iov_base = buf.data();
    local.iov_len = size;
    remote.iov_base = reinterpret_cast<void*>(static_cast<uintptr_t>(address));
    remote.iov_len = size;

    ssize_t n = process_vm_readv(pid, &local, 1, &remote, 1, 0);
    if (n <= 0) {
        env->DeleteLocalRef(result);
        return nullptr;
    }

    env->SetByteArrayRegion(result, 0, static_cast<jsize>(n),
        reinterpret_cast<const jbyte*>(buf.data()));
    return result;
}

/// Direct process_vm_readv helper (used by findModuleBaseNative)
static ssize_t read_remote(pid_t pid, uintptr_t addr, void* buf, size_t len) {
    iovec local{buf, len};
    iovec remote{reinterpret_cast<void*>(addr), len};
    return process_vm_readv(pid, &local, 1, &remote, 1, 0);
}

/// Try to read /proc/<pid>/maps via native open() — sometimes Java FileReader
/// fails due to SELinux but native open() with O_RDONLY succeeds.
/// Returns first match address, or 0 if not found / can't read.
static uintptr_t try_read_maps_native(pid_t pid, const char* module_name) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", pid);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;

    uintptr_t result = 0;
    char buf[8192];
    std::string leftover;

    while (true) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) break;

        leftover.append(buf, n);

        size_t pos;
        while ((pos = leftover.find('\n')) != std::string::npos) {
            std::string line = leftover.substr(0, pos);
            leftover.erase(0, pos + 1);

            if (line.find(module_name) == std::string::npos) continue;

            // Parse: addr_range perms offset dev inode pathname
            uintptr_t start = 0, end = 0;
            char perms[8] = {};
            if (sscanf(line.c_str(), "%lx-%lx %7s", &start, &end, perms) >= 3) {
                if (perms[0] == 'r') {
                    result = start;
                    break;
                }
            }
        }
        if (result) break;
    }

    close(fd);
    return result;
}

/// Scan remote process memory for ELF magic to find a .so module base.
/// Used as fallback when /proc/pid/maps is unreadable.
/// Scans anonymous mapping region (0x7000_0000_0000 ~ 0x8000_0000_0000) in 64KB pages.
/// When ELF header found, validates it's a shared object (ET_DYN) and optionally
/// checks if the SO contains a signature string.
static uintptr_t scan_memory_for_module(pid_t pid, const char* module_name) {
    // ARM64 Android: shared libs typically mapped at 0x7000_0000_0000 ~ 0x7fff_ffff_ffff
    // We scan 4KB-aligned pages looking for ELF magic.
    // To speed up, we read 64KB at a time and search for magic.

    constexpr uintptr_t SCAN_START = 0x700000000000ULL;
    constexpr uintptr_t SCAN_END   = 0x800000000000ULL;
    constexpr size_t SCAN_PAGE = 0x1000;       // 4KB
    constexpr size_t CHUNK_SIZE = 0x10000;     // 64KB

    // Build a short signature from module name to verify
    // We'll read the first 4KB of each ELF and look for the .dynstr section
    // which contains the soname. But that's complex. Instead, we verify:
    // 1. ELF magic (0x7f E L F)
    // 2. ELF class = 2 (64-bit)
    // 3. ELF type = ET_DYN (3, shared object)
    // 4. Try to read program headers and find PT_LOAD with the SO path in .interp or similar
    //
    // For libil2cpp.so specifically, we can check for a known string in the .rodata
    // But the most reliable approach: after finding ELF header, read enough to
    // find the soname in .dynstr via PT_DYNAMIC.

    std::vector<uint8_t> chunk(CHUNK_SIZE);

    for (uintptr_t addr = SCAN_START; addr < SCAN_END; addr += CHUNK_SIZE) {
        ssize_t n = read_remote(pid, addr, chunk.data(), CHUNK_SIZE);
        if (n <= 0) continue;

        size_t readable = static_cast<size_t>(n);

        // Search for ELF magic in this chunk (4-byte aligned)
        for (size_t i = 0; i + sizeof(Elf64_Ehdr) <= readable; i += SCAN_PAGE) {
            // Check ELF magic
            if (chunk[i] != 0x7f || chunk[i+1] != 'E' ||
                chunk[i+2] != 'L' || chunk[i+3] != 'F') continue;

            Elf64_Ehdr* ehdr = reinterpret_cast<Elf64_Ehdr*>(chunk.data() + i);

            // Must be 64-bit, little-endian, ET_DYN (shared object)
            if (ehdr->e_ident[EI_CLASS] != ELFCLASS64) continue;
            if (ehdr->e_ident[EI_DATA] != ELFDATA2LSB) continue;
            if (ehdr->e_type != ET_DYN) continue;
            if (ehdr->e_machine != EM_AARCH64) continue;

            uintptr_t elf_base = addr + i;

            // Validate: read program headers and check PT_DYNAMIC
            // to find soname and match against module_name
            if (ehdr->e_phoff == 0 || ehdr->e_phnum == 0) continue;

            // Read program headers (they might be in the same chunk)
            uintptr_t phdr_addr = elf_base + ehdr->e_phoff;
            size_t phdr_total = ehdr->e_phnum * ehdr->e_phentsize;

            // If program headers extend beyond our chunk, read them separately
            std::vector<uint8_t> phdr_buf;
            uint8_t* phdr_ptr = nullptr;

            size_t phdr_offset_in_chunk = i + ehdr->e_phoff;
            if (phdr_offset_in_chunk + phdr_total <= readable) {
                phdr_ptr = chunk.data() + phdr_offset_in_chunk;
            } else {
                phdr_buf.resize(phdr_total);
                ssize_t pn = read_remote(pid, phdr_addr, phdr_buf.data(), phdr_total);
                if (pn <= 0) continue;
                phdr_ptr = phdr_buf.data();
            }

            // Find PT_DYNAMIC
            uintptr_t dynamic_addr = 0;
            size_t dynamic_size = 0;
            for (int j = 0; j < ehdr->e_phnum; j++) {
                Elf64_Phdr* phdr = reinterpret_cast<Elf64_Phdr*>(
                    phdr_ptr + j * ehdr->e_phentsize);
                if (phdr->p_type == PT_DYNAMIC) {
                    dynamic_addr = elf_base + phdr->p_vaddr;
                    dynamic_size = phdr->p_memsz;
                    break;
                }
            }

            if (!dynamic_addr) continue;

            // Read .dynamic section, find DT_SONAME and DT_STRTAB
            std::vector<uint8_t> dyn_buf(std::min(dynamic_size, (size_t)4096));
            ssize_t dn = read_remote(pid, dynamic_addr, dyn_buf.data(), dyn_buf.size());
            if (dn <= 0) continue;

            uintptr_t strtab_addr = 0;
            uint32_t soname_offset = 0;
            bool has_soname = false;

            size_t dyn_entries = static_cast<size_t>(dn) / sizeof(Elf64_Dyn);
            for (size_t j = 0; j < dyn_entries; j++) {
                Elf64_Dyn* dyn = reinterpret_cast<Elf64_Dyn*>(dyn_buf.data() + j * sizeof(Elf64_Dyn));
                if (dyn->d_tag == DT_NULL) break;
                if (dyn->d_tag == DT_STRTAB) {
                    strtab_addr = static_cast<uintptr_t>(dyn->d_un.d_ptr);
                }
                if (dyn->d_tag == DT_SONAME) {
                    soname_offset = static_cast<uint32_t>(dyn->d_un.d_val);
                    has_soname = true;
                }
            }

            // Try to match soname
            if (has_soname && strtab_addr) {
                // Read soname string from strtab
                char soname[256] = {};
                ssize_t sn = read_remote(pid, strtab_addr + soname_offset, soname, sizeof(soname) - 1);
                if (sn > 0) {
                    soname[sn] = '\0';
                    if (strstr(soname, module_name) != nullptr) {
                        LOGI("Found %s at 0x%lx (soname=%s)", module_name, elf_base, soname);
                        return elf_base;
                    }
                }
            }

            // If no soname match, try matching by reading the first PT_LOAD's
            // content for a signature string (e.g. the module name itself appears
            // in .rodata of most SOs).
            // For libil2cpp.so, a reliable signature is "il2cpp" in .rodata.
            // We check if the module_name appears in the first 4KB of the SO.
            // (Already read in our chunk)
            std::string elf_content(reinterpret_cast<char*>(chunk.data() + i),
                                   std::min(readable - i, (size_t)4096));
            if (elf_content.find(module_name) != std::string::npos) {
                // Additional validation: check if this looks like the right SO
                // by looking for a secondary string
                if (strstr(module_name, "il2cpp") != nullptr) {
                    // libil2cpp.so should contain "il2cpp" in its rodata
                    if (elf_content.find("il2cpp") != std::string::npos ||
                        elf_content.find("IL2CPP") != std::string::npos) {
                        LOGI("Found libil2cpp.so at 0x%lx (string match)", elf_base);
                        return elf_base;
                    }
                } else if (strstr(module_name, "GameCore") != nullptr) {
                    if (elf_content.find("GameCore") != std::string::npos) {
                        LOGI("Found libGameCore.so at 0x%lx (string match)", elf_base);
                        return elf_base;
                    }
                }
            }
        }
    }

    return 0;
}

JNIEXPORT jlong JNICALL
Java_com_esp_core_ShizukuService_nativeFindModuleBase(JNIEnv* env, jclass cls,
        jint pid, jstring module_name) {
    const char* name = env->GetStringUTFChars(module_name, nullptr);
    std::string name_str(name);
    env->ReleaseStringUTFChars(module_name, name);

    LOGI("findModuleBaseNative: pid=%d module=%s", pid, name_str.c_str());

    // Method 1: Try native open() on /proc/pid/maps
    // Sometimes Java FileReader fails but native open succeeds
    uintptr_t base = try_read_maps_native(pid, name_str.c_str());
    if (base) {
        LOGI("findModuleBaseNative: found via native maps read at 0x%lx", base);
        return static_cast<jlong>(base);
    }

    // Method 2: Scan memory for ELF magic
    LOGI("findModuleBaseNative: maps read failed, scanning memory...");
    base = scan_memory_for_module(pid, name_str.c_str());
    if (base) {
        return static_cast<jlong>(base);
    }

    LOGE("findModuleBaseNative: %s not found in pid %d", name_str.c_str(), pid);
    return 0;
}

/// Diagnostic: return a summary of what we can access for a pid
JNIEXPORT jstring JNICALL
Java_com_esp_core_ShizukuService_nativeDiagnosePid(JNIEnv* env, jclass cls, jint pid) {
    std::string diag;

    // Self info
    diag += "=== Shizuku Process Info ===\n";
    diag += "my pid=" + std::to_string(getpid()) + "\n";
    diag += "my uid=" + std::to_string(getuid()) + "\n";
    diag += "target pid=" + std::to_string(pid) + "\n\n";

    // Test 1: Can we read /proc/pid/cmdline?
    diag += "=== /proc/" + std::to_string(pid) + " access ===\n";
    char cmd_path[64];
    snprintf(cmd_path, sizeof(cmd_path), "/proc/%d/cmdline", pid);
    int fd = open(cmd_path, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        char buf[256] = {};
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n > 0) {
            // cmdline may contain null bytes, replace with space for display
            for (ssize_t i = 0; i < n; i++) {
                if (buf[i] == '\0') buf[i] = ' ';
            }
            diag += "cmdline: " + std::string(buf) + "\n";
        } else {
            diag += "cmdline: read failed (errno=" + std::to_string(errno) + ")\n";
        }
    } else {
        diag += "cmdline: open failed (errno=" + std::to_string(errno) + " - " +
                std::string(strerror(errno)) + ")\n";
    }

    // Test 2: Can we read /proc/pid/maps?
    char maps_path[64];
    snprintf(maps_path, sizeof(maps_path), "/proc/%d/maps", pid);
    fd = open(maps_path, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        char buf[4096];
        ssize_t n = read(fd, buf, sizeof(buf));
        close(fd);
        if (n > 0) {
            diag += "maps: readable (" + std::to_string(n) + " bytes first read)\n";
            // Show first 500 chars of maps
            std::string maps_preview(buf, std::min((ssize_t)500, n));
            // Replace newlines with | for compact display
            for (auto& c : maps_preview) if (c == '\n') c = '|';
            diag += "maps preview: " + maps_preview + "...\n";
        } else {
            diag += "maps: open OK but read failed (errno=" + std::to_string(errno) + ")\n";
        }
    } else {
        diag += "maps: open failed (errno=" + std::to_string(errno) + " - " +
                std::string(strerror(errno)) + ")\n";
    }

    // Test 3: process_vm_readv — try multiple addresses
    diag += "\n=== process_vm_readv tests ===\n";

    struct TestAddr {
        const char* name;
        uintptr_t addr;
    };
    TestAddr tests[] = {
        {"0x0 (null)", 0x0},
        {"0x700000000000 (SO region)", 0x700000000000ULL},
        {"0x710000000000", 0x710000000000ULL},
        {"0x720000000000", 0x720000000000ULL},
        {"0x780000000000", 0x780000000000ULL},
        {"0x7fff00000000", 0x7fff00000000ULL},
        {"0x50000000 (low mem)", 0x50000000ULL},
        {"0x80000000", 0x80000000ULL},
    };

    for (const auto& t : tests) {
        uint32_t test_val = 0;
        errno = 0;
        ssize_t rn = read_remote(pid, t.addr, &test_val, 4);
        if (rn > 0) {
            char hex[32];
            snprintf(hex, sizeof(hex), "0x%08x", test_val);
            diag += std::string(t.name) + ": OK (val=" + hex + ")\n";
        } else {
            diag += std::string(t.name) + ": FAIL (errno=" + std::to_string(errno) +
                    " - " + std::string(strerror(errno)) + ")\n";
        }
    }

    // Test 4: /proc/pid/status
    diag += "\n=== /proc/" + std::to_string(pid) + "/status ===\n";
    char status_path[64];
    snprintf(status_path, sizeof(status_path), "/proc/%d/status", pid);
    fd = open(status_path, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        char buf[4096] = {};
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n > 0) {
            std::string status(buf);
            // Extract key lines
            const char* keys[] = {"Name:", "Uid:", "Gid:", "TracerPid:",
                                  "VmSize:", "VmRSS:", "Threads:"};
            for (const char* key : keys) {
                size_t pos = status.find(key);
                if (pos != std::string::npos) {
                    size_t eol = status.find('\n', pos);
                    diag += status.substr(pos, eol - pos) + "\n";
                }
            }
        } else {
            diag += "status: read failed\n";
        }
    } else {
        diag += "status: open failed (errno=" + std::to_string(errno) + ")\n";
    }

    // Test 5: Try reading /proc/pid/maps via cat-like approach (full read)
    diag += "\n=== Full maps read attempt ===\n";
    fd = open(maps_path, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        int total_so = 0;
        int total_lines = 0;
        char buf[8192];
        std::string full_maps;
        while (true) {
            ssize_t n = read(fd, buf, sizeof(buf));
            if (n <= 0) break;
            full_maps.append(buf, n);
        }
        close(fd);
        // Count lines and .so entries
        size_t pos = 0;
        while ((pos = full_maps.find('\n', pos)) != std::string::npos) {
            total_lines++;
            pos++;
        }
        pos = 0;
        while ((pos = full_maps.find(".so", pos)) != std::string::npos) {
            total_so++;
            pos += 3;
        }
        diag += "maps total lines=" + std::to_string(total_lines) + "\n";
        diag += "maps .so mentions=" + std::to_string(total_so) + "\n";
        if (total_lines > 0) {
            // Find first .so line
            pos = full_maps.find(".so");
            if (pos != std::string::npos) {
                size_t line_start = full_maps.rfind('\n', pos);
                if (line_start == std::string::npos) line_start = 0;
                else line_start++;
                size_t line_end = full_maps.find('\n', pos);
                if (line_end == std::string::npos) line_end = full_maps.size();
                diag += "first .so line: " + full_maps.substr(line_start, line_end - line_start) + "\n";
            }
            // Search for il2cpp
            pos = full_maps.find("il2cpp");
            if (pos != std::string::npos) {
                size_t line_start = full_maps.rfind('\n', pos);
                if (line_start == std::string::npos) line_start = 0;
                else line_start++;
                size_t line_end = full_maps.find('\n', pos);
                if (line_end == std::string::npos) line_end = full_maps.size();
                diag += "il2cpp line: " + full_maps.substr(line_start, line_end - line_start) + "\n";
            } else {
                diag += "il2cpp: NOT in maps\n";
            }
        }
    } else {
        diag += "maps: cannot open (errno=" + std::to_string(errno) + " - " +
                std::string(strerror(errno)) + ")\n";
    }

    LOGI("diagnosePid completed:\n%s", diag.c_str());
    return env->NewStringUTF(diag.c_str());
}

} // extern "C"
