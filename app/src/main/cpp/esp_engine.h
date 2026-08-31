#pragma once

#include "memory_reader.h"
#include "il2cpp_resolver.h"
#include "offset_resolver.h"
#include "offset_table.h"
#include <android/log.h>
#include <array>
#include <vector>
#include <mutex>
#include <atomic>
#include <thread>
#include <condition_variable>

#define ESP_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "ESP", __VA_ARGS__)
#define ESP_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "ESP", __VA_ARGS__)

namespace esp {

/// 4x4 matrix (column-major, like Unity/OpenGL)
struct Matrix4x4 {
    float m[16];
};

/// 3D vector
struct Vec3 {
    float x, y, z;
};

/// 2D screen point
struct ScreenPoint {
    float x, y;
    bool visible;  // true if in front of camera
};

/// Entity data for rendering
struct EntityInfo {
    Vec3 position;       // World position
    ScreenPoint screen;  // Projected screen position
    int32_t camp;        // 0 = ally, 1 = enemy (varies by game)
    int32_t actor_type;  // ACTOR_TYPE_HERO = 1, etc.
    int32_t hp;          // Current HP
    int32_t max_hp;      // Max HP
    std::string name;    // Actor name (optional)
};

/// Configuration for the ESP
struct EspConfig {
    bool show_enemies = true;
    bool show_allies = false;
    bool show_minions = false;
    bool show_jungle = false;
    bool show_hp_bars = true;
    bool show_names = true;
    bool show_distance = true;
    bool show_boxes = true;
    bool show_lines = false;      // Lines from bottom of screen to entities
    float box_color_enemy[4] = {1.0f, 0.0f, 0.0f, 1.0f};   // Red
    float box_color_ally[4] = {0.0f, 1.0f, 0.0f, 1.0f};     // Green
    int update_rate_ms = 16;      // ~60fps
};

/// Core ESP engine. Reads game memory and computes screen positions.
/// Runs in a background thread, updates entity list periodically.
class EspEngine {
public:
    EspEngine();
    ~EspEngine();

    /// Initialize with the game package name (direct mode).
    /// Returns false if the game process is not found or metadata can't be loaded.
    bool init(const std::string& game_package);

    /// Initialize with a pre-configured MemoryReader (Shizuku or direct mode).
    /// The reader must already have the correct PID / module bases set.
    bool init(std::unique_ptr<MemoryReader> reader,
              uintptr_t il2cpp_base, uintptr_t gamecore_base);

    /// Load a revx-produced offset table (offsets.json). Optional: when absent
    /// the engine falls back to its runtime heuristics. Returns false when the
    /// file exists but fails validation.
    bool load_offset_table(const std::string& path);

    /// True if a validated offset table is in use.
    bool has_offset_table() const { return offset_table_ != nullptr; }

    /// Set screen dimensions (called from Java).
    void set_screen_size(int w, int h) {
        screen_width_ = w;
        screen_height_ = h;
    }

    /// Start the background update thread.
    void start();

    /// Stop the background update thread.
    void stop();

    /// Get the current entity list (thread-safe copy).
    std::vector<EntityInfo> get_entities() const;

    /// Get current config.
    EspConfig get_config() const;
    void set_config(const EspConfig& config);

    /// Check if the engine is running.
    bool is_running() const { return running_.load(); }

    /// Get screen dimensions (from the game's surface).
    int screen_width() const { return screen_width_; }
    int screen_height() const { return screen_height_; }

private:
    void update_loop();

    /// Read the camera VP matrix from the game.
    bool read_camera_matrix(Matrix4x4& view, Matrix4x4& proj);

    /// Offset-table path through read_camera_matrix: Camera.current via the
    /// table's static_fields_offset, then table-supplied matrix offsets.
    /// Returns false (heuristics continue) on any table miss.
    bool read_camera_matrix_via_table(uintptr_t camera_klass, Matrix4x4& view, Matrix4x4& proj);

    /// Scan for game entities (heroes, minions, etc.)
    bool scan_entities(std::vector<EntityInfo>& entities);

    /// Project a world position to screen coordinates.
    ScreenPoint world_to_screen(const Vec3& world, const Matrix4x4& view, const Matrix4x4& proj) const;

    /// Resolve a field offset, caching results.
    uint32_t resolve_field(uintptr_t klass, const std::string& name);

    // State
    std::unique_ptr<MemoryReader> reader_;
    std::unique_ptr<Il2cppResolver> resolver_;
    std::unique_ptr<OffsetResolver> offset_resolver_;
    uintptr_t il2cpp_base_ = 0;
    uintptr_t gamecore_base_ = 0;

    // Resolved offsets (cached)
    std::unordered_map<std::string, uint32_t> field_cache_;
    std::mutex cache_mutex_;

    // revx-produced offset table (optional; nullptr = heuristic mode)
    std::unique_ptr<OffsetTable> offset_table_;

    // Camera
    uintptr_t camera_klass_ = 0;
    uint32_t camera_world_to_camera_matrix_offset_ = 0;
    uint32_t camera_projection_matrix_offset_ = 0;

    // Actor system
    uintptr_t actor_manager_ = 0;

    // Screen dimensions
    int screen_width_ = 1080;
    int screen_height_ = 1920;

    // Entity list (double-buffered)
    mutable std::mutex entities_mutex_;
    std::vector<EntityInfo> entities_;

    // Config
    mutable std::mutex config_mutex_;
    EspConfig config_;

    // Thread control
    std::atomic<bool> running_{false};
    std::thread update_thread_;
};

} // namespace esp
