#pragma once

#include "memory_reader.h"
#include "il2cpp_resolver.h"
#include <unordered_map>
#include <string>

namespace esp {

/// Resolves runtime offsets by analyzing the target process's code.
/// This runs on-device (in the overlay app's process) and reads the
/// game process's memory to extract class pointers and field offsets.
///
/// The key challenge: il2cpp stores class pointers in a runtime table
/// (s_TypeInfoDefinitionTable) that's populated during il2cpp_init().
/// We find this table by analyzing the exported il2cpp functions' assembly.
class OffsetResolver {
public:
    OffsetResolver(const MemoryReader& reader, uintptr_t il2cpp_base)
        : reader_(reader), il2cpp_base_(il2cpp_base) {}

    /// Find the s_TypeInfoDefinitionTable pointer by analyzing
    /// il2cpp_class_from_name's code.
    /// Returns the remote address of the table, or 0 on failure.
    uintptr_t find_type_info_table();

    /// Find the s_domain pointer by analyzing il2cpp_domain_get's code.
    uintptr_t find_domain_ptr();

    /// Find the metadata cache (s_GlobalMetadata) by analyzing
    /// il2cpp_init or MetadataCache::Initialize.
    uintptr_t find_metadata_cache();

    /// Get a class pointer from the type info table by index.
    uintptr_t get_class_from_table(uintptr_t table, int32_t type_index);

    /// Find a class by scanning the type info table for a matching name.
    /// This is slow (O(n)) but works without calling any remote functions.
    uintptr_t find_class_by_name(const std::string& namespace_name,
                                  const std::string& class_name,
                                  uintptr_t type_table, int32_t table_size);

private:
    /// Decode an ADRP+ADD/LDR pair to get the target address.
    /// Returns the absolute address, or 0 if the instructions don't match.
    uintptr_t decode_adrp_target(uintptr_t instruction_addr);

    /// Read a 4-byte ARM64 instruction.
    uint32_t read_inst(uintptr_t addr) {
        uint32_t inst = 0;
        reader_.read_t(addr, inst);
        return inst;
    }

    const MemoryReader& reader_;
    uintptr_t il2cpp_base_;

    // Cached addresses
    uintptr_t cached_domain_ = 0;
    uintptr_t cached_type_table_ = 0;
    uintptr_t cached_metadata_ = 0;
};

} // namespace esp
