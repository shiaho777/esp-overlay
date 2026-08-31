#include "esp_engine.h"

#include <cmath>
#include <algorithm>

namespace esp {

EspEngine::EspEngine() = default;

EspEngine::~EspEngine() {
    stop();
}

bool EspEngine::init(const std::string& game_package) {
    // Find the game process
    pid_t pid = MemoryReader::find_pid(game_package);
    if (pid <= 0) {
        return false;
    }

    reader_ = std::make_unique<MemoryReader>(pid);

    // Find libil2cpp.so base address
    il2cpp_base_ = reader_->get_module_base("libil2cpp.so");
    if (!il2cpp_base_) {
        return false;
    }

    // Find libGameCore.so base address
    gamecore_base_ = reader_->get_module_base("libGameCore.so");

    // Initialize il2cpp resolver
    resolver_ = std::make_unique<Il2cppResolver>(*reader_, il2cpp_base_);
    if (!resolver_->init()) {
        return false;
    }

    // Initialize offset resolver for dynamic class lookup
    offset_resolver_ = std::make_unique<OffsetResolver>(*reader_, il2cpp_base_);

    return true;
}

bool EspEngine::init(std::unique_ptr<MemoryReader> reader,
                     uintptr_t il2cpp_base, uintptr_t gamecore_base) {
    reader_ = std::move(reader);
    il2cpp_base_ = il2cpp_base;
    gamecore_base_ = gamecore_base;

    // Initialize il2cpp resolver
    resolver_ = std::make_unique<Il2cppResolver>(*reader_, il2cpp_base_);
    if (!resolver_->init()) {
        return false;
    }

    // Initialize offset resolver for dynamic class lookup
    offset_resolver_ = std::make_unique<OffsetResolver>(*reader_, il2cpp_base_);

    return true;
}

void EspEngine::start() {
    if (running_.exchange(true)) return;
    update_thread_ = std::thread(&EspEngine::update_loop, this);
}

void EspEngine::stop() {
    running_ = false;
    if (update_thread_.joinable()) {
        update_thread_.join();
    }
}

std::vector<EntityInfo> EspEngine::get_entities() const {
    std::lock_guard<std::mutex> lock(entities_mutex_);
    return entities_;
}

EspConfig EspEngine::get_config() const {
    std::lock_guard<std::mutex> lock(config_mutex_);
    return config_;
}

void EspEngine::set_config(const EspConfig& config) {
    std::lock_guard<std::mutex> lock(config_mutex_);
    config_ = config;
}

uint32_t EspEngine::resolve_field(uintptr_t klass, const std::string& name) {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    auto it = field_cache_.find(name);
    if (it != field_cache_.end()) return it->second;

    uint32_t offset = resolver_->get_field_offset(klass, name);
    if (offset > 0) {
        field_cache_[name] = offset;
    }
    return offset;
}

bool EspEngine::load_offset_table(const std::string& path) {
    auto table = std::make_unique<OffsetTable>();
    std::string error;
    if (!OffsetTable::load(path, *table, error)) {
        ESP_LOGE("offset table load failed: %s", error.c_str());
        return false;
    }
    ESP_LOGI("offset table loaded: binary=%s version=%d classes=%zu globals=%zu methods=%zu",
             table->binary_name().c_str(), table->table_version(),
             table->class_count(), table->global_count(), table->method_count());
    offset_table_ = std::move(table);
    return true;
}

void EspEngine::update_loop() {
    while (running_) {
        EspConfig cfg = get_config();

        Matrix4x4 view, proj;
        bool has_camera = read_camera_matrix(view, proj);

        std::vector<EntityInfo> new_entities;
        if (has_camera && scan_entities(new_entities)) {
            // Project all entities to screen
            for (auto& e : new_entities) {
                e.screen = world_to_screen(e.position, view, proj);
            }

            // Filter by config
            new_entities.erase(std::remove_if(new_entities.begin(), new_entities.end(),
                [&](const EntityInfo& e) {
                    if (!e.screen.visible) return true;
                    if (e.actor_type == 1) { // ACTOR_TYPE_HERO
                        if (e.camp == 1 && !cfg.show_enemies) return true;
                        if (e.camp == 0 && !cfg.show_allies) return true;
                    }
                    if (e.actor_type == 3 && !cfg.show_minions) return true;  // soldier
                    if (e.actor_type == 4 && !cfg.show_jungle) return true;   // monster
                    return false;
                }), new_entities.end());
        }

        {
            std::lock_guard<std::mutex> lock(entities_mutex_);
            entities_ = std::move(new_entities);
        }

        // Sleep for update rate
        std::this_thread::sleep_for(std::chrono::milliseconds(cfg.update_rate_ms));
    }
}

bool EspEngine::read_camera_matrix(Matrix4x4& view, Matrix4x4& proj) {
    // Use OffsetResolver to dynamically find the Camera class and read VP matrices.
    //
    // Flow:
    // 1. Find s_TypeInfoDefinitionTable by analyzing il2cpp_class_from_name's code
    // 2. Scan the table for UnityEngine.Camera class (by matching name)
    // 3. Read Camera's static fields → Camera.current pointer
    // 4. Read worldToCameraMatrix and projectionMatrix from the camera object

    if (!offset_resolver_ || !reader_) return false;

    // Find the type info table (cached after first call)
    uintptr_t type_table = offset_resolver_->find_type_info_table();
    if (!type_table) return false;

    // Find Camera class by scanning the type table
    // Camera.current is a static field; we need the class pointer to access it
    if (!camera_klass_) {
        camera_klass_ = offset_resolver_->find_class_by_name(
            "UnityEngine", "Camera", type_table, 50000);
        if (!camera_klass_) return false;
    }

    // Offset-table fast path: static_fields_offset and matrix offsets come from
    // the revx-produced table; on any miss fall through to the version heuristics.
    if (offset_table_) {
        Matrix4x4 table_view, table_proj;
        if (read_camera_matrix_via_table(camera_klass_, table_view, table_proj)) {
            view = table_view;
            proj = table_proj;
            return true;
        }
    }

    // Get static fields data pointer
    uintptr_t static_data = 0;
    if (!reader_->read_t(camera_klass_ + 0x68, static_data) || !static_data) {
        return false;
    }

    // Find the "current" static field offset.
    // In il2cpp, Camera.current is backed by a private static field.
    // We scan the class's fields for one named "current" (or similar).
    // For Unity 2021 il2cpp, the field is typically named "currentCamera"
    // or accessed via get_current() which reads from a static field.
    //
    // FieldInfo array is at Il2CppClass.fields (offset ~0x78)
    // Field count is at Il2CppClass.field_count (offset ~0x88)
    uintptr_t fields_ptr = 0;
    uint16_t field_count = 0;
    reader_->read_t(camera_klass_ + 0x78, fields_ptr);
    reader_->read_t(camera_klass_ + 0x88, field_count);
    if (!fields_ptr || field_count == 0 || field_count > 256) return false;

    // FieldInfo: { const char* name (0x00); Il2CppType* type (0x08); uint32_t offset (0x18); }
    // Size = 0x20 on 64-bit
    uint32_t current_field_offset = 0;
    for (uint16_t i = 0; i < field_count; i++) {
        uintptr_t field_addr = fields_ptr + i * 0x20;
        uintptr_t name_ptr = 0;
        if (!reader_->read_t(field_addr, name_ptr) || !name_ptr) continue;
        std::string fname = reader_->read_string(name_ptr, 128);
        // Camera's static "current" field
        if (fname == "current" || fname == "currentCamera" || fname == "mainCamera") {
            reader_->read_t(field_addr + 0x18, current_field_offset);
            break;
        }
    }

    if (!current_field_offset) return false;

    // Read Camera.current pointer from static data
    uintptr_t camera_obj = 0;
    if (!reader_->read_t(static_data + current_field_offset, camera_obj) || !camera_obj) {
        return false;
    }

    // Read VP matrices from the camera object.
    // Unity Camera (MonoBehaviour → Behaviour → Component → Object):
    //   worldToCameraMatrix: offset varies by Unity version
    //   projectionMatrix: offset = worldToCameraMatrix + 0x40
    //
    // For Unity 2021.x (il2cpp 29.x):
    //   worldToCameraMatrix at camera_obj + 0xD8 (64 bytes)
    //   projectionMatrix at camera_obj + 0x118 (64 bytes)
    //
    // We try multiple known offsets and validate by checking matrix values.

    struct MatrixOffset { uint32_t view_off; uint32_t proj_off; };
    MatrixOffset candidates[] = {
        {0xD8, 0x118},  // Unity 2021.x
        {0xB8, 0xF8},   // Unity 2020.x
        {0xC0, 0x100},  // Unity 2019.x
        {0xC8, 0x108},  // Unity 2018.x
    };

    for (const auto& cand : candidates) {
        if (!reader_->read(camera_obj + cand.view_off, view.m, sizeof(view.m))) continue;
        if (!reader_->read(camera_obj + cand.proj_off, proj.m, sizeof(proj.m))) continue;

        // Validate: projection matrix [3][2] should be -1 (perspective)
        // In column-major: proj.m[11] = proj.m[3*4 + 2] = -1.0f
        if (proj.m[11] < -0.5f && proj.m[11] > -2.0f) {
            return true;
        }
        // Also try [2][3] in row-major interpretation
        if (proj.m[14] < -0.5f && proj.m[14] > -2.0f) {
            return true;
        }
    }

    return false;
}

bool EspEngine::read_camera_matrix_via_table(uintptr_t camera_klass, Matrix4x4& view,
                                             Matrix4x4& proj) {
    if (!offset_table_ || !reader_) return false;

    const FieldOffset* current =
        offset_table_->find_field("UnityEngine.Camera", "current");
    if (current == nullptr || current->offset == 0) return false;

    uint32_t static_fields_offset = 0;
    if (const ClassOffsets* camera = offset_table_->find_class("UnityEngine.Camera")) {
        if (camera->has_static_fields) {
            static_fields_offset = camera->static_fields_offset;
        }
    }

    uintptr_t static_data = 0;
    if (!reader_->read_t(camera_klass + 0x68, static_data) || !static_data) {
        return false;
    }
    if (static_fields_offset) {
        // The table's static_fields_offset is the Il2CppClass member offset
        // where the static-fields pointer lives; re-deref from the class.
        static_data = 0;
        if (!reader_->read_t(camera_klass + static_fields_offset, static_data) || !static_data) {
            return false;
        }
    }

    uintptr_t camera_obj = 0;
    if (!reader_->read_t(static_data + current->offset, camera_obj) || !camera_obj) {
        return false;
    }

    const GlobalOffset* view_off_global = offset_table_->find_global("UnityEngine.Camera.worldToCameraMatrix");
    const GlobalOffset* proj_off_global = offset_table_->find_global("UnityEngine.Camera.projectionMatrix");
    if (view_off_global == nullptr || proj_off_global == nullptr) return false;

    uint32_t view_off = static_cast<uint32_t>(view_off_global->address);
    uint32_t proj_off = static_cast<uint32_t>(proj_off_global->address);
    if (view_off == 0 || proj_off == 0) return false;

    if (!reader_->read(camera_obj + view_off, view.m, sizeof(view.m))) return false;
    if (!reader_->read(camera_obj + proj_off, proj.m, sizeof(proj.m))) return false;

    // Same perspective sanity check as the heuristic path.
    if (proj.m[11] < -0.5f && proj.m[11] > -2.0f) return true;
    if (proj.m[14] < -0.5f && proj.m[14] > -2.0f) return true;
    return false;
}

bool EspEngine::scan_entities(std::vector<EntityInfo>& entities) {
    // Strategy: Use Unity's Object system to find all game objects with
    // the appropriate component (Hero, Actor, etc.).
    //
    // Alternative: Read GameCore's ActorManager directly.
    // The ActorManager is accessed via:
    //   GameRoot::ActiveDesk()->BattleLogic->ActorManager
    //
    // GameRoot is a singleton stored in libGameCore.so's .data segment.
    // We find it by analyzing the string reference:
    //   "GameRoot::ActiveDesk()->BattleLogic->isFighting" at 0x10facf0
    // The function that references this string contains the path to GameRoot.
    //
    // GameRoot singleton is typically a static pointer at a fixed .data offset.
    // We analyze ModuleInit (exported at 0x420ff74) which is a thunk:
    //   adrp x8, page; ldr x0, [x8, #0x3e8]; br x0
    // The actual init function is called through this thunk and stores
    // the GameRoot singleton.

    if (!reader_ || !gamecore_base_) return false;

    // The GameCore uses a component buffer architecture:
    //   PushCsBuffer / CommitCsBuffer (C# → C++)
    //   PushViewBuffer / CommitViewBuffer (C++ → C#)
    //
    // Actor data is likely accessible through the view buffer system.
    // Each frame, GameCore commits view data that includes actor positions.
    //
    // For the overlay, we can read:
    // 1. Camera VP matrix (from il2cpp/Unity)
    // 2. Actor positions from the view buffer or ActorManager
    //
    // The view buffer is committed via CommitViewBuffer (exported at 0x421d894).
    // It writes to a shared buffer that C# reads. We can find the buffer
    // pointer by analyzing this function.

    // For a production implementation, the approach would be:
    // a) Hook into the view buffer commit (but we can't hook — TSS detects it)
    // b) Read the view buffer after it's committed (race condition)
    // c) Read the ActorManager directly (best approach)
    //
    // ActorManager path:
    //   libGameCore.so .data → GameRoot* s_gameRoot
    //   s_gameRoot->activeDesk → GameDesk*
    //   GameDesk->battleLogic → CBattleLogic*
    //   CBattleLogic->actorManager → CActorManager*
    //   CActorManager->actorList → Actor[] (array of Actor*)
    //   Actor->position (Vec3), camp (int), type (int), hp (int)
    //
    // The s_gameRoot offset in .data is found by:
    // 1. Analyzing the "GameRoot::ActiveDesk" string reference
    // 2. Finding the function that loads s_gameRoot
    // 3. Extracting the ADRP+LDR offset from that function

    // For now, this returns false. The offset resolution requires
    // dynamic analysis on the target device or more in-depth static analysis.

    return false;
}

ScreenPoint EspEngine::world_to_screen(const Vec3& world, const Matrix4x4& view, const Matrix4x4& proj) const {
    // Standard World → View → Projection → Screen transform
    // VP = Projection × View (matrix multiplication)
    // Then: clip = VP × (world, 1)
    // NDC = clip.xyz / clip.w
    // Screen = ((NDC.x+1)*W/2, (1-NDC.y)*H/2)

    Matrix4x4 vp;
    // Matrix multiply: proj × view (column-major)
    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            float sum = 0;
            for (int k = 0; k < 4; k++) {
                // proj is column-major: proj.m[col*4 + row]
                // view is column-major: view.m[k*4 + row]
                // result[col*4 + row] = sum_k proj[k*4+row] * view[col*4+k]
                sum += proj.m[k * 4 + row] * view.m[col * 4 + k];
            }
            vp.m[col * 4 + row] = sum;
        }
    }

    // Apply VP to world position
    float clip_x = vp.m[0] * world.x + vp.m[4] * world.y + vp.m[8]  * world.z + vp.m[12];
    float clip_y = vp.m[1] * world.x + vp.m[5] * world.y + vp.m[9]  * world.z + vp.m[13];
    float clip_z = vp.m[2] * world.x + vp.m[6] * world.y + vp.m[10] * world.z + vp.m[14];
    float clip_w = vp.m[3] * world.x + vp.m[7] * world.y + vp.m[11] * world.z + vp.m[15];

    ScreenPoint result{0, 0, false};

    if (clip_w <= 0.001f) {
        // Behind camera
        return result;
    }

    // Perspective divide → NDC
    float inv_w = 1.0f / clip_w;
    float ndc_x = clip_x * inv_w;
    float ndc_y = clip_y * inv_w;
    float ndc_z = clip_z * inv_w;

    // NDC to screen
    result.x = (ndc_x + 1.0f) * 0.5f * static_cast<float>(screen_width_);
    result.y = (1.0f - ndc_y) * 0.5f * static_cast<float>(screen_height_);
    result.visible = (ndc_z >= -1.0f && ndc_z <= 1.0f);

    return result;
}

} // namespace esp
