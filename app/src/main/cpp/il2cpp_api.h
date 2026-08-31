#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <optional>

namespace esp {

// ---- il2cpp runtime type definitions (from IL2CPP source) ----

struct Il2CppClass;
struct Il2CppType;
struct MethodInfo;
struct Il2CppImage;
struct Il2CppDomain;
struct Il2CppAssembly;
struct FieldInfo;

// Il2CppClass layout (simplified, offsets matter for remote reading)
// This is the IL2CPP open-source struct; games rarely change it.
struct Il2CppClassRemote {
    // --- offsets are architecture-specific but stable across il2cpp versions ---
    const char* name;               // +0x00  (pointer to class name)
    const char* namespaze;          // +0x08
    Il2CppClass* parent;            // +0x10
    Il2CppType* byval_arg;          // +0x18
    // ... many fields ...
    // The exact layout depends on il2cpp version. We use metadata instead
    // of hardcoded offsets for class structure.
};

/// Metadata about a class loaded from global-metadata.dat
struct Il2CppTypeDefinitionInfo {
    std::string name;
    std::string namespace_name;
    std::string assembly_name;
    uint32_t token = 0;
    // Field offsets are resolved at runtime by reading the class's field info
};

/// Represents a resolved field with its offset in the object.
struct ResolvedField {
    std::string name;
    uint32_t offset = 0;  // Offset within the object instance
    std::string type_name;
};

/// Represents a resolved il2cpp class with field offsets.
struct ResolvedClass {
    std::string name;
    std::string namespace_name;
    uintptr_t klass_ptr = 0;    // Remote pointer to Il2CppClass
    std::vector<ResolvedField> fields;
    uintptr_t static_fields_ptr = 0; // Remote pointer to static field data
};

} // namespace esp
