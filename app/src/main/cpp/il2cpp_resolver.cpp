#include "il2cpp_resolver.h"

#include <cstring>
#include <fstream>
#include <sstream>

namespace esp {

// global-metadata.dat magic
static constexpr int32_t METADATA_SANITY = 0xFAB11BAF;

Il2cppResolver::Il2cppResolver(const MemoryReader& reader, uintptr_t il2cpp_base)
    : reader_(reader), il2cpp_base_(il2cpp_base) {}

const char* Il2cppResolver::get_string(int32_t index) const {
    if (!header_ || index < 0) return "";
    uintptr_t offset = header_->stringOffset + index;
    if (offset >= metadata_.size()) return "";
    return reinterpret_cast<const char*>(metadata_.data() + offset);
}

const Il2CppTypeDefinition* Il2cppResolver::get_type_definition(size_t index) const {
    if (!header_) return nullptr;
    uintptr_t offset = header_->typeDefinitionsOffset + index * sizeof(Il2CppTypeDefinition);
    if (offset + sizeof(Il2CppTypeDefinition) > metadata_.size()) return nullptr;
    return reinterpret_cast<const Il2CppTypeDefinition*>(metadata_.data() + offset);
}

size_t Il2cppResolver::type_definition_count() const {
    if (!header_) return 0;
    return header_->typeDefinitionsSize / sizeof(Il2CppTypeDefinition);
}

const Il2CppFieldDefinition* Il2cppResolver::get_field_definition(size_t index) const {
    if (!header_) return nullptr;
    uintptr_t offset = header_->fieldsOffset + index * sizeof(Il2CppFieldDefinition);
    if (offset + sizeof(Il2CppFieldDefinition) > metadata_.size()) return nullptr;
    return reinterpret_cast<const Il2CppFieldDefinition*>(metadata_.data() + offset);
}

bool Il2cppResolver::init() {
    // Try to find global-metadata.dat on the local filesystem first.
    // On a real device, this would be in /data/data/<package>/files/ or
    // extracted from the APK. For development, we can also read it from
    // the remote process's memory.

    // Method 1: Try common paths on device
    std::vector<std::string> candidate_paths = {
        "/data/data/com.tencent.tmgp.sgame/files/il2cpp_data/Metadata/global-metadata.dat",
        "/data/data/com.tencent.tmgp.sgame.il2cpp/files/il2cpp_data/Metadata/global-metadata.dat",
        "/sdcard/Android/data/com.tencent.tmgp.sgame/files/il2cpp_data/Metadata/global-metadata.dat",
    };

    for (const auto& path : candidate_paths) {
        if (load_metadata_from_file(path)) {
            return true;
        }
    }

    // Method 2: Scan the remote process memory for the metadata magic.
    // The metadata is loaded into memory by il2cpp_init().
    // We search the .data segment of libil2cpp.so for a pointer to it,
    // or scan anonymous mappings for the magic bytes.

    // Read the .data segment of libil2cpp.so
    std::string maps_path = "/proc/" + std::to_string(reader_.pid()) + "/maps";
    std::ifstream maps(maps_path);
    if (!maps.is_open()) return false;

    std::string line;
    while (std::getline(maps, line)) {
        // Look for anonymous mappings or libil2cpp data segments
        if (line.find("libil2cpp.so") == std::string::npos &&
            line.find("[anon:") == std::string::npos) {
            // Also check for the metadata in the heap
            if (line.find("[heap]") == std::string::npos &&
                line.find("anon") == std::string::npos) {
                continue;
            }
        }

        uintptr_t start = 0, end = 0;
        char perms[5] = {};
        if (sscanf(line.c_str(), "%lx-%lx %4s", &start, &end, perms) < 3) continue;
        if (perms[0] != 'r') continue;

        // Scan this region for the metadata magic in chunks
        size_t region_size = end - start;
        if (region_size > 64 * 1024 * 1024) continue; // Skip huge regions

        // Read in 1MB chunks
        size_t chunk_size = std::min<size_t>(region_size, 1024 * 1024);
        std::vector<uint8_t> chunk(chunk_size);

        for (uintptr_t addr = start; addr + sizeof(int32_t) <= end; addr += chunk_size - sizeof(int32_t)) {
            size_t to_read = std::min<size_t>(chunk_size, end - addr);
            if (!reader_.read(addr, chunk.data(), to_read)) break;

            // Search for magic
            for (size_t i = 0; i + sizeof(int32_t) <= to_read; i += 4) {
                int32_t sanity = *reinterpret_cast<const int32_t*>(chunk.data() + i);
                if (sanity != METADATA_SANITY) continue;

                // Potential metadata header. Read a larger block to verify.
                size_t meta_size = std::min<size_t>(to_read - i, 64 * 1024 * 1024);
                metadata_.resize(meta_size);
                if (!reader_.read(addr + i, metadata_.data(), meta_size)) {
                    metadata_.clear();
                    continue;
                }

                header_ = reinterpret_cast<const Il2CppGlobalMetadataHeader*>(metadata_.data());

                // Validate header
                if (header_->version >= 16 &&
                    header_->stringOffset < (int32_t)meta_size &&
                    header_->typeDefinitionsOffset < (int32_t)meta_size &&
                    header_->typeDefinitionsSize > 0) {
                    // Trim metadata to actual size
                    // The actual size is the last offset + size, but we don't know
                    // all fields. Keep what we have.
                    return true;
                }

                metadata_.clear();
                header_ = nullptr;
            }
        }
    }

    return false;
}

bool Il2cppResolver::load_metadata_from_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return false;

    f.seekg(0, std::ios::end);
    size_t size = f.tellg();
    f.seekg(0, std::ios::beg);

    if (size < sizeof(Il2CppGlobalMetadataHeader)) return false;

    metadata_.resize(size);
    f.read(reinterpret_cast<char*>(metadata_.data()), size);

    header_ = reinterpret_cast<const Il2CppGlobalMetadataHeader*>(metadata_.data());

    if (header_->sanity != METADATA_SANITY) {
        metadata_.clear();
        header_ = nullptr;
        return false;
    }

    return true;
}

uintptr_t Il2cppResolver::find_class(const std::string& namespace_name, const std::string& class_name) {
    if (!header_) return 0;

    // Search through type definitions for a matching name
    size_t count = type_definition_count();
    for (size_t i = 0; i < count; i++) {
        const Il2CppTypeDefinition* td = get_type_definition(i);
        if (!td) continue;

        const char* name = get_string(td->nameIndex);
        const char* ns = get_string(td->namespaceIndex);

        if (class_name == name && namespace_name == ns) {
            // Found the type definition. Now we need to find the runtime Il2CppClass pointer.
            // In il2cpp, the runtime class is stored in a global table indexed by type index.
            // We use the il2cpp_class_from_name exported function instead.

            // The type index in metadata corresponds to an Il2CppType with il2cpp_type_index.
            // The actual Il2CppClass* is obtained at runtime via:
            //   il2cpp::vm::MetadataCache::GetTypeInfoFromTypeDefinitionIndex(index)

            // For remote reading, we call the exported il2cpp_class_from_name function
            // by reading its code and finding the global metadata cache pointer.
            //
            // Alternative: scan the il2cpp type table in .data segment.
            // The table is: Il2CppClass** s_TypeInfoDefinitionTable
            // It's populated during il2cpp_init().

            // For now, return the type definition index encoded as a pseudo-pointer.
            // The caller will use il2cpp_class_from_name via a different path.
            return static_cast<uintptr_t>(i + 1); // Non-zero = found
        }
    }

    return 0;
}

uintptr_t Il2cppResolver::get_static_fields(uintptr_t klass) {
    // Read the static_fields pointer from the Il2CppClass struct
    uintptr_t static_data = 0;
    if (!reader_.read_t(klass + Il2CppClassLayout::STATIC_FIELDS_OFFSET, static_data)) {
        return 0;
    }
    return static_data;
}

uint32_t Il2cppResolver::get_field_offset(uintptr_t klass, const std::string& field_name) {
    // In il2cpp, field offsets are stored in the FieldInfo array.
    // FieldInfo layout: { const char* name; Il2CppType* type; uint32_t offset; Il2CppClass* parent; }
    // The offset of FieldInfo::offset depends on il2cpp version.
    // For il2cpp 24.x (Unity 2018+): offset = 0x18 (after name ptr, type ptr)
    // For il2cpp 27.x (Unity 2020+): offset = 0x18 (same)
    // For il2cpp 29.x (Unity 2021+): offset = 0x18 (same)

    // The fields array pointer is at Il2CppClass.fields (offset ~0x78-0x80, varies).
    // field_count is at offset ~0x90.

    // Due to layout variability, we use a search approach:
    // Read a chunk of the class struct and search for field name pointers.

    // Read class struct (256 bytes should cover the header)
    uint8_t class_buf[256];
    if (!reader_.read(klass, class_buf, sizeof(class_buf))) return 0;

    // Try to read fields pointer and count from known offsets
    // These offsets work for il2cpp 24.x - 29.x on ARM64
    uintptr_t fields_ptr = *reinterpret_cast<uintptr_t*>(class_buf + 0x78);
    uint16_t field_count = *reinterpret_cast<uint16_t*>(class_buf + 0x88);

    if (!fields_ptr || field_count == 0 || field_count > 512) return 0;

    // FieldInfo size is 0x20 (32 bytes) on 64-bit
    constexpr size_t FIELD_INFO_SIZE = 0x20;
    constexpr size_t FIELD_NAME_OFFSET = 0x00;
    constexpr size_t FIELD_OFFSET_VAL = 0x18;

    for (uint16_t i = 0; i < field_count; i++) {
        uintptr_t field_addr = fields_ptr + i * FIELD_INFO_SIZE;

        // Read field name pointer
        uintptr_t name_ptr = 0;
        if (!reader_.read_t(field_addr + FIELD_NAME_OFFSET, name_ptr) || !name_ptr) continue;

        // Read field name string
        std::string name = reader_.read_string(name_ptr, 256);
        if (name == field_name) {
            // Read the offset value
            uint32_t offset = 0;
            if (reader_.read_t(field_addr + FIELD_OFFSET_VAL, offset)) {
                return offset;
            }
        }
    }

    return 0;
}

std::string Il2cppResolver::get_class_name(uintptr_t klass) {
    uintptr_t name_ptr = 0;
    if (!reader_.read_t(klass + Il2CppClassLayout::NAME_OFFSET, name_ptr) || !name_ptr) {
        return "";
    }
    return reader_.read_string(name_ptr, 256);
}

std::string Il2cppResolver::get_namespace(uintptr_t klass) {
    uintptr_t ns_ptr = 0;
    if (!reader_.read_t(klass + Il2CppClassLayout::NAMESPACE_OFFSET, ns_ptr) || !ns_ptr) {
        return "";
    }
    return reader_.read_string(ns_ptr, 256);
}

} // namespace esp
