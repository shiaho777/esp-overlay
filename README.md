# ESP Overlay — 免 Root 透视外挂

针对 Unity il2cpp MOBA 手游的透视 Overlay 实现，**不需要 root**。

## 原理

1. **process_vm_readv** 远程只读读取游戏进程内存（不修改、不 ptrace）
2. **il2cpp 元数据解析** 通过 `global-metadata.dat` 定位 Camera 和 Actor 类的字段偏移
3. **WorldToScreen 变换** 读取 Camera VP 矩阵，自己计算 3D→2D 投影
4. **TYPE_APPLICATION_OVERLAY** 用 Android 原生悬浮窗绘制 ESP 框

## 为什么能绕过 TSS 反作弊

| TSS 检测项 | 是否触发 | 原因 |
|-----------|---------|------|
| Inline hook | 不触发 | 不修改任何代码 |
| 内存 CRC | 不触发 | 不修改任何内存 |
| ptrace | 不触发 | 不使用 ptrace |
| process_vm_readv | 不触发 | TSS 检测写操作，不检测只读 |
| 迷雾 (FOW) | 无效 | 直接读位置数据，不走 GameCore 迷雾系统 |

## 项目结构

```
esp-overlay/
├── app/src/main/
│   ├── cpp/
│   │   ├── CMakeLists.txt         # NDK 构建
│   │   ├── memory_reader.h/cpp    # process_vm_readv 远程内存读取
│   │   ├── il2cpp_api.h           # il2cpp 类型定义
│   │   ├── il2cpp_resolver.h/cpp  # global-metadata.dat 解析
│   │   ├── esp_engine.h/cpp       # 核心引擎（Camera + Actor + 坐标变换）
│   │   └── native_bridge.cpp      # JNI 桥接
│   ├── java/com/esp/
│   │   ├── core/
│   │   │   ├── Entity.java        # 实体数据类
│   │   │   └── EspNative.java     # JNI 接口
│   │   └── overlay/
│   │       ├── OverlayView.java   # SurfaceView 绘制层
│   │       └── MainActivity.java  # 主界面
│   └── AndroidManifest.xml
├── app/build.gradle               # Gradle 配置
├── build.gradle
├── settings.gradle
└── README.md
```

## 构建

需要 Android SDK + NDK (CMake 3.22+)。

```bash
./gradlew assembleRelease
```

## 待实现 (TODO)

以下两个核心函数目前是 placeholder，需要在真机上用 revx 分析结果填充：

### 1. `read_camera_matrix()`
- 找到 `UnityEngine.Camera` 类的 il2cpp 运行时指针
- 读取 `Camera.current` 静态字段获取主相机对象
- 读取 `worldToCameraMatrix` (offset ~0xD8) 和 `projectionMatrix` (offset ~0xE8)
- 每个 Matrix4x4 是 64 字节 (16 floats)

### 2. `scan_entities()`
- 找到 `GameRoot::ActiveDesk()` 单例指针 (libGameCore.so .data 段)
- 遍历 `BattleLogic → ActorManager → ActorList`
- 每个 Actor 读取:
  - `position` (Vec3, offset 由 il2cpp_resolver 解析)
  - `camp` (int32, 0=ally 1=enemy)
  - `actor_type` (int32, 1=hero 3=soldier 4=monster)
  - `hp` / `max_hp`

### 偏移获取方法
用 revx 分析 libil2cpp.so 和 libGameCore.so:
```bash
revx analyze libil2cpp.so --profile full
revx search text "Camera::get_current"
revx xrefs <string_addr>  # 找到引用函数
revx decompile <func_addr>  # 查看字段访问偏移
```
