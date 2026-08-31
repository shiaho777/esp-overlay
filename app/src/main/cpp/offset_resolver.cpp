#include "offset_resolver.h"
#include <cmath>

namespace esp {

uintptr_t OffsetResolver::decode_adrp_target(uintptr_t instruction_addr) {
    uint32_t adrp = read_inst(instruction_addr);
    if ((adrp & 0x9F000000) != 0x90000000) return 0;

    // ADRP: imm = sign_extend(immhi:immlo << 12)
    int64_t immhi = (adrp >> 5) & 0x7FFFF;
    int64_t immlo = (adrp >> 29) & 0x3;
    int64_t imm = (immhi << 2) | immlo;
    if (imm & (1LL << 20)) imm |= ~(0x1FFFFFLL);

    return (instruction_addr & ~0xFFFULL) + (imm << 12);
}

uintptr_t OffsetResolver::find_domain_ptr() {
    if (cached_domain_) return cached_domain_;

    // il2cpp_domain_get is at il2cpp_base + 0x3a8af60
    // Typical pattern:
    //   adrp x8, <page>
    //   ldr x0, [x8, #<offset>]
    //   ret
    uintptr_t func_addr = il2cpp_base_ + 0x3a8af60;

    uintptr_t page = decode_adrp_target(func_addr);
    if (!page) return 0;

    uint32_t ldr = read_inst(func_addr + 4);
    if ((ldr & 0xFFC00000) != 0xF9400000) return 0;

    uint32_t offset = ((ldr >> 10) & 0xFFF) * 8;
    uintptr_t domain_ptr_addr = page + offset;

    // Read the actual domain pointer
    uintptr_t domain = 0;
    if (!reader_.read_t(domain_ptr_addr, domain) || !domain) return 0;

    cached_domain_ = domain;
    return domain;
}

uintptr_t OffsetResolver::find_type_info_table() {
    if (cached_type_table_) return cached_type_table_;

    // The type info table (s_TypeInfoDefinitionTable) is accessed by
    // il2cpp_class_from_name and other functions.
    //
    // il2cpp_class_from_name is at il2cpp_base + 0x3a8a900
    // We analyze its code to find the table.
    //
    // The function signature: Il2CppClass* il2cpp_class_from_name(
    //     const Il2CppImage* image, const char* namespaze, const char* name)
    //
    // It calls MetadataCache::GetTypeInfoFromTypeDefinitionIndex internally.
    // The table is a global: Il2CppClass** s_TypeInfoDefinitionTable
    //
    // We scan the function's first ~50 instructions for ADRP+LDR patterns
    // that load a pointer from .bss/.data, then verify by checking if the
    // loaded value looks like a class pointer.

    uintptr_t func_addr = il2cpp_base_ + 0x3a8a900;

    // Scan up to 50 instructions for ADRP + LDR patterns
    for (int i = 0; i < 50; i++) {
        uintptr_t addr = func_addr + i * 4;
        uint32_t inst = read_inst(addr);

        // Look for ADRP
        if ((inst & 0x9F000000) != 0x90000000) continue;

        uintptr_t page = decode_adrp_target(addr);
        if (!page) continue;

        // Check next instruction for LDR
        uint32_t next = read_inst(addr + 4);
        if ((next & 0xFFC00000) == 0xF9400000) {
            // LDR x, [xN, #imm]
            uint32_t offset = ((next >> 10) & 0xFFF) * 8;
            uintptr_t table_addr = page + offset;

            // Read the table pointer
            uintptr_t table = 0;
            if (reader_.read_t(table_addr, table) && table) {
                // Verify: read first entry, check if it looks like a class pointer
                uintptr_t first_class = 0;
                if (reader_.read_t(table, first_class) && first_class > 0x1000) {
                    // Read class name pointer to verify
                    uintptr_t name_ptr = 0;
                    if (reader_.read_t(first_class, name_ptr) && name_ptr > 0x1000) {
                        std::string name = reader_.read_string(name_ptr, 128);
                        if (!name.empty()) {
                            cached_type_table_ = table;
                            return table;
                        }
                    }
                }
            }
        }
    }

    return 0;
}

uintptr_t OffsetResolver::find_metadata_cache() {
    if (cached_metadata_) return cached_metadata_;

    // s_GlobalMetadata is accessed by MetadataCache::Initialize
    // and many other functions. We can find it by analyzing
    // il2cpp_image_get_class_count or similar functions.
    //
    // il2cpp_image_get_class_count is at il2cpp_base + 0x3a8baec
    // Its code accesses the metadata's image count.

    uintptr_t func_addr = il2cpp_base_ + 0x3a8baec;

    for (int i = 0; i < 30; i++) {
        uintptr_t addr = func_addr + i * 4;
        uint32_t inst = read_inst(addr);

        if ((inst & 0x9F000000) != 0x90000000) continue;

        uintptr_t page = decode_adrp_target(addr);
        if (!page) continue;

        uint32_t next = read_inst(addr + 4);
        if ((next & 0xFFC00000) == 0xF9400000) {
            uint32_t offset = ((next >> 10) & 0xFFF) * 8;
            uintptr_t meta_ptr_addr = page + offset;

            uintptr_t meta = 0;
            if (reader_.read_t(meta_ptr_addr, meta) && meta > 0x1000) {
                cached_metadata_ = meta;
                return meta;
            }
        }
    }

    return 0;
}

uintptr_t OffsetResolver::get_class_from_table(uintptr_t table, int32_t type_index) {
    if (!table || type_index < 0) return 0;
    uintptr_t klass = 0;
    reader_.read_t(table + type_index * sizeof(uintptr_t), klass);
    return klass;
}

uintptr_t OffsetResolver::find_class_by_name(const std::string& namespace_name,
                                               const std::string& class_name,
                                               uintptr_t type_table,
                                               int32_t table_size) {
    if (!type_table) return 0;

    // Iterate the type info table
    for (int32_t i = 0; i < table_size && i < 100000; i++) {
        uintptr_t klass = get_class_from_table(type_table, i);
        if (!klass || klass < 0x1000) continue;

        // Read class name
        uintptr_t name_ptr = 0;
        if (!reader_.read_t(klass, name_ptr) || !name_ptr) continue;
        std::string name = reader_.read_string(name_ptr, 256);
        if (name != class_name) continue;

        // Read namespace
        uintptr_t ns_ptr = 0;
        if (!reader_.read_t(klass + 8, ns_ptr) || !ns_ptr) continue;
        std::string ns = reader_.read_string(ns_ptr, 256);
        if (ns == namespace_name) {
            return klass;
        }
    }

    return 0;
}

} // namespace esp
