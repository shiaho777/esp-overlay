#pragma once

#include "memory_reader.h"
#include "il2cpp_api.h"
#include <unordered_map>
#include <mutex>

namespace esp {

/// il2cpp global-metadata.dat header structure.
/// Based on the open-source IL2CPP runtime (il2cpp-class-internals.h).
struct Il2CppGlobalMetadataHeader {
    int32_t sanity;            // 0xFAB11BAF
    int32_t version;
    int32_t stringLiteralOffset;
    int32_t stringLiteralSize;
    int32_t stringLiteralDataOffset;
    int32_t stringLiteralDataSize;
    int32_t stringOffset;
    int32_t stringSize;
    int32_t eventsOffset;
    int32_t eventsSize;
    int32_t propertiesOffset;
    int32_t propertiesSize;
    int32_t methodsOffset;
    int32_t methodsSize;
    int32_t parameterDefaultValuesOffset;
    int32_t parameterDefaultValuesSize;
    int32_t fieldDefaultValuesOffset;
    int32_t fieldDefaultValuesSize;
    int32_t fieldAndParameterDefaultValueDataOffset;
    int32_t fieldAndParameterDefaultValueDataSize;
    int32_t fieldMarshaledSizesOffset;
    int32_t fieldMarshaledSizesSize;
    int32_t parametersOffset;
    int32_t parametersSize;
    int32_t fieldsOffset;
    int32_t fieldsSize;
    int32_t genericParametersOffset;
    int32_t genericParametersSize;
    int32_t genericParameterConstraintsOffset;
    int32_t genericParameterConstraintsSize;
    int32_t genericContainersOffset;
    int32_t genericContainersSize;
    int32_t nestedTypesOffset;
    int32_t nestedTypesSize;
    int32_t interfacesOffset;
    int32_t interfacesSize;
    int32_t vtableMethodsOffset;
    int32_t vtableMethodsSize;
    int32_t interfaceOffsetsOffset;
    int32_t interfaceOffsetsSize;
    int32_t typeDefinitionsOffset;
    int32_t typeDefinitionsSize;
    int32_t imagesOffset;
    int32_t imagesSize;
    int32_t assembliesOffset;
    int32_t assembliesSize;
    // ... more fields in newer versions
};

// TypeDefinition (Il2CppTypeDefinition) - from global-metadata.dat
// This is the key structure that contains field layout info.
struct Il2CppTypeDefinition {
    int32_t nameIndex;          // index into string table
    int32_t namespaceIndex;     // index into string table
    int32_t byvalTypeIndex;
    int32_t declaringTypeIndex;
    int32_t parentIndex;
    int32_t elementTypeIndex;
    int32_t genericContainerIndex;
    uint32_t flags;
    int32_t fieldStart;         // index into fields array
    int32_t methodStart;        // index into methods array
    int32_t eventStart;
    int32_t propertyStart;
    int32_t nestedTypesStart;
    int32_t interfacesStart;
    int32_t vtableStart;
    int32_t interfaceOffsetsStart;
    uint16_t method_count;
    uint16_t property_count;
    uint16_t field_count;
    uint16_t event_count;
    uint16_t nested_type_count;
    uint16_t vtable_count;
    uint16_t interfaces_count;
    uint16_t interface_offsets_count;
    uint32_t bitfield;
    uint32_t token;
};

// Field definition in metadata
struct Il2CppFieldDefinition {
    int32_t nameIndex;     // index into string table
    int32_t typeIndex;
    uint32_t token;
};

/// Il2CppClass partial layout for remote reading.
/// We only need specific offsets. These are stable for il2cpp 24.x-29.x (Unity 2018-2022).
struct Il2CppClassLayout {
    // The actual Il2CppClass struct in memory. Offsets verified against
    // il2cpp source code. These are for 64-bit ARM.
    static constexpr size_t NAME_OFFSET = 0x00;          // const char* name
    static constexpr size_t NAMESPACE_OFFSET = 0x08;     // const char* namespaze
    static constexpr size_t PARENT_OFFSET = 0x10;        // Il2CppClass* parent
    static constexpr size_t BYVAL_ARG_OFFSET = 0x18;     // Il2CppType byval_arg (inline)
    static constexpr size_t THIS_ARG_OFFSET = 0x28;      // Il2CppType this_arg (inline)
    static constexpr size_t ELEMENT_CLASS_OFFSET = 0x30; // Il2CppClass* element_class
    static constexpr size_t CAST_CLASS_OFFSET = 0x38;    // Il2CppClass* castClass
    static constexpr size_t NATURAL_SIZE_OFFSET = 0x50;  // uint32_t
    static constexpr size_t INSTANCE_SIZE_OFFSET = 0x54; // uint32_t
    static constexpr size_t ACTUAL_SIZE_OFFSET = 0x58;   // uint32_t
    static constexpr size_t SIZE_INCLUDING_ALIGNMENT = 0x5C; // uint32_t
    static constexpr size_t STATIC_FIELDS_OFFSET = 0x68; // void* static_fields (ptr to static data)
    static constexpr size_t FIELD_COUNT_OFFSET = 0x90;   // uint16_t field_count (approx, varies)
};

/// Resolves il2cpp class information by reading the remote process memory.
/// Uses global-metadata.dat for type definitions and runtime class pointers
/// for field offsets.
class Il2cppResolver {
public:
    Il2cppResolver(const MemoryReader& reader, uintptr_t il2cpp_base);

    /// Initialize: find global-metadata.dat in the remote process,
    /// parse the header and type definitions.
    bool init();

    /// Find a class by namespace and name.
    /// Returns a remote pointer to the Il2CppClass, or 0 if not found.
    uintptr_t find_class(const std::string& namespace_name, const std::string& class_name);

    /// Get the static fields data pointer for a class.
    uintptr_t get_static_fields(uintptr_t klass);

    /// Read a static field value from a class.
    template <typename T>
    bool read_static_field(uintptr_t klass, uint32_t offset, T& out) {
        uintptr_t static_data = get_static_fields(klass);
        if (!static_data) return false;
        return reader_.read_t(static_data + offset, out);
    }

    /// Find a field offset by name within a class.
    /// This uses the il2cpp_field_get_offset approach: read the FieldInfo array.
    uint32_t get_field_offset(uintptr_t klass, const std::string& field_name);

    /// Read the class name from a remote Il2CppClass pointer.
    std::string get_class_name(uintptr_t klass);

    /// Read the namespace from a remote Il2CppClass pointer.
    std::string get_namespace(uintptr_t klass);

    const MemoryReader& reader() const { return reader_; }
    uintptr_t il2cpp_base() const { return il2cpp_base_; }

private:
    const MemoryReader& reader_;
    uintptr_t il2cpp_base_;

    // global-metadata.dat data (read from the remote process or local file)
    std::vector<uint8_t> metadata_;
    const Il2CppGlobalMetadataHeader* header_ = nullptr;

    // String table accessor
    const char* get_string(int32_t index) const;

    // Type definitions
    const Il2CppTypeDefinition* get_type_definition(size_t index) const;
    size_t type_definition_count() const;

    // Field definitions
    const Il2CppFieldDefinition* get_field_definition(size_t index) const;

    // Find the metadata file path in the remote process
    std::string find_metadata_path();

    // Try to read metadata from the APK's extracted data directory
    bool load_metadata_from_file(const std::string& path);
};

} // namespace esp
