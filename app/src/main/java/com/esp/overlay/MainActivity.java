package com.esp.overlay;

import android.app.Activity;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.provider.Settings;
import android.text.method.ScrollingMovementMethod;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import com.esp.core.EspNative;
import com.esp.core.ShizukuManager;
import com.esp.core.ShizukuMemoryReader;

/**
 * Main activity.
 *
 * 使用流程:
 * 1. 安装 Shizuku App（从 https://shizuku.rikka.app/ 下载）
 * 2. 用 ADB 启动 Shizuku:
 *    adb shell sh /storage/emulated/0/Android/data/moe.shizuku.privileged.api/start.sh
 * 3. 打开王者荣耀，进入游戏
 * 4. 打开 ESP Overlay，授予 Shizuku 权限
 * 5. 点 START ESP
 */
public class MainActivity extends Activity {

    private static final int OVERLAY_PERMISSION_CODE = 1001;
    private static final String GAME_PACKAGE = "com.tencent.tmgp.sgame";

    private OverlayView overlayView;
    private CheckBox cbEnemies, cbAllies, cbMinions, cbJungle;
    private CheckBox cbHp, cbNames, cbDistance, cbBoxes, cbLines;
    private Button btnToggle;
    private Button btnShizuku;
    private TextView statusText;
    private boolean running = false;

    private ShizukuManager shizukuManager;

    /// 完整日志缓冲区 — 收集所有步骤的输出，供复制
    private final StringBuilder logBuffer = new StringBuilder();
    private Button btnCopyLog;
    private TextView logView;
    private ScrollView logScroll;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        shizukuManager = new ShizukuManager(this);
        shizukuManager.addListeners();

        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setPadding(32, 32, 32, 32);

        // Title
        TextView title = new TextView(this);
        title.setText("ESP Overlay — 免Root透视");
        title.setTextSize(20);
        title.setPadding(0, 0, 0, 16);
        layout.addView(title);

        // Status (简洁状态)
        statusText = new TextView(this);
        statusText.setText("状态: 准备就绪");
        statusText.setTextSize(14);
        statusText.setPadding(0, 0, 0, 8);
        layout.addView(statusText);

        // Shizuku button
        btnShizuku = new Button(this);
        btnShizuku.setText("检查 Shizuku 状态");
        btnShizuku.setOnClickListener(v -> checkShizuku());
        layout.addView(btnShizuku);

        // Config section
        TextView configTitle = new TextView(this);
        configTitle.setText("显示选项:");
        configTitle.setTextSize(16);
        configTitle.setPadding(0, 8, 0, 4);
        layout.addView(configTitle);

        cbEnemies = createCheckbox(layout, "显示敌人", true);
        cbAllies = createCheckbox(layout, "显示队友", false);
        cbMinions = createCheckbox(layout, "显示小兵", false);
        cbJungle = createCheckbox(layout, "显示野怪", false);
        cbHp = createCheckbox(layout, "显示血条", true);
        cbNames = createCheckbox(layout, "显示名称", false);
        cbDistance = createCheckbox(layout, "显示距离", true);
        cbBoxes = createCheckbox(layout, "显示方框", true);
        cbLines = createCheckbox(layout, "显示连线", false);

        Button btnApply = new Button(this);
        btnApply.setText("应用配置");
        btnApply.setOnClickListener(v -> applyConfig());
        layout.addView(btnApply);

        btnToggle = new Button(this);
        btnToggle.setText("START ESP");
        btnToggle.setTextSize(16);
        btnToggle.setOnClickListener(v -> toggleEsp());
        layout.addView(btnToggle);

        // --- 完整日志区域 ---
        TextView logTitle = new TextView(this);
        logTitle.setText("=== 完整日志 ===");
        logTitle.setTextSize(14);
        logTitle.setPadding(0, 16, 0, 4);
        layout.addView(logTitle);

        btnCopyLog = new Button(this);
        btnCopyLog.setText("复制全部日志");
        btnCopyLog.setOnClickListener(v -> copyLogToClipboard());
        layout.addView(btnCopyLog);

        logView = new TextView(this);
        logView.setTextSize(12);
        logView.setTypeface(android.graphics.Typeface.MONOSPACE);
        logView.setMovementMethod(new ScrollingMovementMethod());
        logView.setText("（暂无日志）");
        logView.setMinHeight(600);
        logView.setMaxHeight(800);
        logView.setPadding(16, 8, 16, 8);
        logView.setBackgroundColor(0xFF1E1E1E);
        logView.setTextColor(0xFFCCCCCC);

        logScroll = new ScrollView(this);
        logScroll.addView(logView);
        layout.addView(logScroll);

        // Instructions
        TextView helpText = new TextView(this);
        helpText.setText("\n使用说明:\n"
            + "1. 安装Shizuku App并通过ADB启动\n"
            + "2. 打开王者荣耀，进入游戏\n"
            + "3. 点「检查Shizuku状态」，确保显示已连接\n"
            + "4. 点 START ESP\n"
            + "5. 切回游戏查看透视\n");
        helpText.setTextSize(13);
        helpText.setPadding(0, 16, 0, 0);
        layout.addView(helpText);

        ScrollView outerScroll = new ScrollView(this);
        outerScroll.addView(layout);
        setContentView(outerScroll);

        overlayView = new OverlayView(this);

        if (!checkOverlayPermission()) {
            requestOverlayPermission();
        }

        updateShizukuStatus();
    }

    // --- 日志系统 ---

    private void appendLog(String msg) {
        if (msg == null || msg.isEmpty()) return;
        logBuffer.append(msg);
        if (!msg.endsWith("\n")) logBuffer.append("\n");
        String full = logBuffer.toString();
        runOnUiThread(() -> {
            logView.setText(full);
            logScroll.post(() -> logScroll.fullScroll(ScrollView.FOCUS_DOWN));
        });
    }

    private void appendLogOnUiThread(String msg) {
        if (msg == null || msg.isEmpty()) return;
        logBuffer.append(msg);
        if (!msg.endsWith("\n")) logBuffer.append("\n");
        String full = logBuffer.toString();
        logView.setText(full);
        logScroll.post(() -> logScroll.fullScroll(ScrollView.FOCUS_DOWN));
    }

    private void clearLog() {
        logBuffer.setLength(0);
        logView.setText("（已清空）");
    }

    private void copyLogToClipboard() {
        String text = logBuffer.toString();
        if (text.isEmpty()) {
            Toast.makeText(this, "日志为空", Toast.LENGTH_SHORT).show();
            return;
        }
        ClipboardManager clipboard = (ClipboardManager) getSystemService(Context.CLIPBOARD_SERVICE);
        if (clipboard != null) {
            ClipData clip = ClipData.newPlainText("ESP Log", text);
            clipboard.setPrimaryClip(clip);
            Toast.makeText(this, "已复制 " + text.length() + " 字符到剪贴板", Toast.LENGTH_SHORT).show();
        } else {
            Toast.makeText(this, "无法访问剪贴板", Toast.LENGTH_SHORT).show();
        }
    }

    // ---

    private CheckBox createCheckbox(LinearLayout layout, String label, boolean checked) {
        CheckBox cb = new CheckBox(this);
        cb.setText(label);
        cb.setChecked(checked);
        layout.addView(cb);
        return cb;
    }

    private void applyConfig() {
        if (overlayView != null) {
            overlayView.updateConfig(
                cbEnemies.isChecked(), cbAllies.isChecked(),
                cbMinions.isChecked(), cbJungle.isChecked(),
                cbHp.isChecked(), cbNames.isChecked(),
                cbDistance.isChecked(), cbBoxes.isChecked(),
                cbLines.isChecked()
            );
            Toast.makeText(this, "配置已应用", Toast.LENGTH_SHORT).show();
        }
    }

    private void checkShizuku() {
        if (!shizukuManager.isAvailable()) {
            appendLogOnUiThread("[Shizuku] 未运行\n请先安装Shizuku App并用ADB启动:\n"
                + "adb shell sh /sdcard/Android/data/moe.shizuku.privileged.api/start.sh");
            statusText.setText("状态: Shizuku未运行");
            return;
        }

        appendLogOnUiThread("[Shizuku] binder可用");

        if (!shizukuManager.hasPermission()) {
            appendLogOnUiThread("[Shizuku] 需要授权，正在请求...");
            statusText.setText("状态: 需要Shizuku权限");
            shizukuManager.requestPermission();
            return;
        }

        appendLogOnUiThread("[Shizuku] 已授权");

        // Bind to service
        if (shizukuManager.bind()) {
            appendLogOnUiThread("[Shizuku] bindUserService 已调用，等待连接...");
            statusText.setText("状态: Shizuku已连接 ✓");
        } else {
            appendLogOnUiThread("[Shizuku] bindUserService 失败!");
            statusText.setText("状态: 绑定失败");
        }
    }

    private void updateShizukuStatus() {
        if (!shizukuManager.isAvailable()) {
            btnShizuku.setText("Shizuku未运行 — 点击查看");
        } else if (!shizukuManager.hasPermission()) {
            btnShizuku.setText("Shizuku未授权 — 点击授权");
        } else {
            btnShizuku.setText("Shizuku已就绪 ✓");
        }
    }

    private void toggleEsp() {
        if (!checkOverlayPermission()) {
            statusText.setText("状态: 需要悬浮窗权限");
            requestOverlayPermission();
            return;
        }

        if (!running) {
            startEsp();
        } else {
            stopEsp();
        }
    }

    private void startEsp() {
        clearLog();
        appendLogOnUiThread("========== ESP 启动 ==========");
        appendLogOnUiThread("时间: " + new java.text.SimpleDateFormat("yyyy-MM-dd HH:mm:ss",
            java.util.Locale.getDefault()).format(new java.util.Date()));
        appendLogOnUiThread("包名: " + GAME_PACKAGE);
        appendLogOnUiThread("");

        // Step 1: Check Shizuku
        if (!shizukuManager.isAvailable()) {
            appendLogOnUiThread("[ERROR] Shizuku未运行");
            appendLogOnUiThread("请先安装并启动Shizuku:");
            appendLogOnUiThread("adb shell sh /sdcard/Android/data/moe.shizuku.privileged.api/start.sh");
            statusText.setText("状态: Shizuku未运行");
            return;
        }
        appendLog("[OK] Shizuku binder可用\n");

        if (!shizukuManager.hasPermission()) {
            appendLog("[WARN] Shizuku未授权，正在请求...\n");
            statusText.setText("状态: 需要Shizuku权限");
            shizukuManager.requestPermission();
            return;
        }
        appendLog("[OK] Shizuku已授权\n");

        // Step 2: Bind Shizuku service (async)
        if (!shizukuManager.isBound()) {
            appendLog("[Step] 正在连接Shizuku服务...\n");
            statusText.setText("状态: 正在连接Shizuku...");
            shizukuManager.bind();
            // Wait for connection in a background thread
            new Thread(() -> {
                int retries = 0;
                while (!shizukuManager.isBound() && retries < 20) {
                    try { Thread.sleep(100); } catch (InterruptedException e) { break; }
                    retries++;
                }
                final int finalRetries = retries;
                runOnUiThread(() -> {
                    if (shizukuManager.isBound()) {
                        appendLogOnUiThread("[OK] Shizuku服务已连接 (重试" + finalRetries + "次)");
                        proceedWithInit();
                    } else {
                        appendLogOnUiThread("[ERROR] Shizuku连接超时 (重试20次 x 100ms)");
                        appendLogOnUiThread("请点「检查Shizuku状态」后重试");
                        statusText.setText("状态: Shizuku连接超时");
                    }
                });
            }).start();
            return;
        }

        appendLog("[OK] Shizuku服务已连接\n");
        proceedWithInit();
    }

    private void proceedWithInit() {
        appendLog("\n--- Step 3: 查找游戏进程 ---\n");
        statusText.setText("状态: 正在查找游戏进程...");

        new Thread(() -> {
            long t0 = System.currentTimeMillis();
            int pid = shizukuManager.findPid(GAME_PACKAGE);
            long dt = System.currentTimeMillis() - t0;
            appendLog("[findPid] " + GAME_PACKAGE + " → PID=" + pid + " (" + dt + "ms)\n");

            if (pid <= 0) {
                appendLog("[ERROR] 游戏未运行！\n");
                appendLog("请先打开王者荣耀并进入游戏界面\n");
                statusText.setText("状态: 游戏未运行");
                return;
            }

            appendLog("[OK] 找到游戏进程 PID=" + pid + "\n");
            statusText.setText("状态: 找到PID=" + pid + "，正在获取模块基址...");

            // --- 诊断 ---
            appendLog("\n--- Step 3.5: 诊断目标进程 ---\n");
            long td = System.currentTimeMillis();
            String diag = shizukuManager.diagnosePid(pid);
            appendLog("[diagnosePid] (" + (System.currentTimeMillis() - td) + "ms)\n");
            appendLog(diag);
            appendLog("\n");

            // --- Step 4a: Java 层 getModuleBase (读 /proc/pid/maps) ---
            appendLog("--- Step 4a: Java getModuleBase (读 /proc/pid/maps) ---\n");
            long il2cppBase = 0;
            String foundIl2cppName = null;
            String[] il2cppNames = {"libil2cpp.so", "libil2cpp.sym.so", "libUnityIL2CPP.so"};
            for (String name : il2cppNames) {
                long t1 = System.currentTimeMillis();
                il2cppBase = shizukuManager.getModuleBase(pid, name);
                long ms = System.currentTimeMillis() - t1;
                appendLog("[getModuleBase] " + name + " → 0x" + Long.toHexString(il2cppBase) + " (" + ms + "ms)\n");
                if (il2cppBase != 0) {
                    foundIl2cppName = name;
                    break;
                }
            }

            long gamecoreBase = 0;
            if (il2cppBase != 0) {
                appendLog("[OK] maps可读，继续找 gamecore\n");
                String[] gcNames = {"libGameCore.so", "libgamecore.so", "libGameCore.sym.so"};
                for (String name : gcNames) {
                    long t1 = System.currentTimeMillis();
                    gamecoreBase = shizukuManager.getModuleBase(pid, name);
                    long ms = System.currentTimeMillis() - t1;
                    appendLog("[getModuleBase] " + name + " → 0x" + Long.toHexString(gamecoreBase) + " (" + ms + "ms)\n");
                    if (gamecoreBase != 0) break;
                }
            } else {
                appendLog("[FAIL] maps不可读 (Permission denied)\n");
            }

            // --- Step 4b: Native 内存扫描 fallback ---
            if (il2cppBase == 0) {
                appendLog("\n--- Step 4b: Native 内存扫描 (process_vm_readv 找 ELF) ---\n");
                appendLog("扫描范围: 0x700000000000 ~ 0x800000000000 (1TB)\n");
                appendLog("预计耗时 10-30秒...\n");
                statusText.setText("状态: 内存扫描中... (10-30秒)");

                for (String name : il2cppNames) {
                    long t1 = System.currentTimeMillis();
                    il2cppBase = shizukuManager.findModuleBaseNative(pid, name);
                    long ms = System.currentTimeMillis() - t1;
                    if (il2cppBase == -2) {
                        appendLog("[findModuleBaseNative] " + name + " → NATIVE_NOT_LOADED (esp-native.so未加载到Shizuku进程)\n");
                        il2cppBase = 0;
                    } else {
                        appendLog("[findModuleBaseNative] " + name + " → 0x" + Long.toHexString(il2cppBase) + " (" + ms + "ms)\n");
                    }
                    if (il2cppBase != 0) {
                        foundIl2cppName = name + " (memscan)";
                        break;
                    }
                }

                if (il2cppBase != 0) {
                    appendLog("[OK] 内存扫描找到模块!\n");
                    String[] gcNames = {"libGameCore.so", "libgamecore.so", "libGameCore.sym.so"};
                    for (String name : gcNames) {
                        long t1 = System.currentTimeMillis();
                        gamecoreBase = shizukuManager.findModuleBaseNative(pid, name);
                        long ms = System.currentTimeMillis() - t1;
                        appendLog("[findModuleBaseNative] " + name + " → 0x" + Long.toHexString(gamecoreBase) + " (" + ms + "ms)\n");
                        if (gamecoreBase != 0) break;
                    }
                } else {
                    appendLog("[FAIL] 内存扫描未找到 ELF 模块\n");
                }
            }

            if (il2cppBase == 0) {
                appendLog("\n========== 最终失败 ==========\n");
                appendLog("PID=" + pid + "\n");
                appendLog("gamecore=" + (gamecoreBase == 0 ? "未找到" : "0x" + Long.toHexString(gamecoreBase)) + "\n\n");
                appendLog("尝试过的方案:\n");
                appendLog("1. Java getModuleBase → Permission denied\n");
                appendLog("2. Native findModuleBaseNative (ELF扫描) → 未找到\n\n");
                appendLog("可能原因:\n");
                appendLog("- 游戏加固壳修改了内存布局\n");
                appendLog("- SO以匿名映射加载(maps无文件名)\n");
                appendLog("- process_vm_readv 无权限\n");
                appendLog("- 需要root权限\n");
                appendLog("\n请点「复制全部日志」按钮复制日志后发给开发者\n");
                statusText.setText("状态: 失败 — 见日志");
                return;
            }

            // Step 5: Set up Shizuku memory reader
            appendLog("\n--- Step 5: 设置内存读取器 ---\n");
            ShizukuMemoryReader memReader = new ShizukuMemoryReader(shizukuManager, pid);
            EspNative.setMemoryReader(memReader);
            appendLog("[OK] ShizukuMemoryReader 已创建 (pid=" + pid + ")\n");

            // Step 6: Init native engine
            String il2cppName = foundIl2cppName;
            final long finalIl2cppBase = il2cppBase;
            appendLog("\n--- Step 6: 初始化引擎 ---\n");
            appendLog("il2cpp=" + il2cppName + " @ 0x" + Long.toHexString(il2cppBase) + "\n");
            appendLog("gamecore=" + (gamecoreBase == 0 ? "(none)" : "0x" + Long.toHexString(gamecoreBase)) + "\n");
            statusText.setText("状态: 初始化引擎中...");

            long ti = System.currentTimeMillis();
            boolean ok = EspNative.initWithShizuku(pid, il2cppBase, gamecoreBase);
            long initMs = System.currentTimeMillis() - ti;
            appendLog("[initWithShizuku] → " + (ok ? "OK" : "FAIL") + " (" + initMs + "ms)\n");

            if (!ok) {
                String error = EspNative.getLastError();
                if (error == null || error.isEmpty()) error = "未知错误";
                appendLog("[ERROR] 引擎初始化失败: " + error + "\n");
                statusText.setText("状态: 失败 — " + error);
                return;
            }

            // Step 7: Start ESP
            appendLog("\n--- Step 7: 启动ESP ---\n");
            final long finalGamecoreBase = gamecoreBase;
            runOnUiThread(() -> {
                applyConfig();
                EspNative.start();
                overlayView.attachToWindow();
                running = true;
                btnToggle.setText("STOP ESP");
                statusText.setText("ESP运行中 PID=" + pid);
                appendLogOnUiThread("[OK] ESP已启动!");
                appendLogOnUiThread("il2cpp=0x" + Long.toHexString(finalIl2cppBase));
                appendLogOnUiThread("gamecore=" + (finalGamecoreBase == 0 ? "(none)" : "0x" + Long.toHexString(finalGamecoreBase)));
                appendLogOnUiThread("\n切回游戏查看透视效果");
            });
        }).start();
    }

    private void stopEsp() {
        EspNative.stop();
        overlayView.detachFromWindow();
        running = false;
        btnToggle.setText("START ESP");
        statusText.setText("状态: 已停止");
        appendLogOnUiThread("[INFO] ESP已停止");
    }

    private boolean checkOverlayPermission() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            return Settings.canDrawOverlays(this);
        }
        return true;
    }

    private void requestOverlayPermission() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            Intent intent = new Intent(
                Settings.ACTION_MANAGE_OVERLAY_PERMISSION,
                Uri.parse("package:" + getPackageName())
            );
            startActivityForResult(intent, OVERLAY_PERMISSION_CODE);
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == OVERLAY_PERMISSION_CODE) {
            if (checkOverlayPermission()) {
                Toast.makeText(this, "悬浮窗权限已授予", Toast.LENGTH_SHORT).show();
            } else {
                Toast.makeText(this, "需要悬浮窗权限", Toast.LENGTH_LONG).show();
            }
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        updateShizukuStatus();
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        if (running) {
            EspNative.stop();
            overlayView.detachFromWindow();
        }
        shizukuManager.removeListeners();
        shizukuManager.unbind();
    }
}
