package com.esp.core;

import android.content.ComponentName;
import android.content.Context;
import android.content.ServiceConnection;
import android.content.pm.PackageManager;
import android.os.IBinder;
import android.os.RemoteException;
import android.util.Log;

import rikka.shizuku.Shizuku;

/**
 * Manages the Shizuku connection for privileged memory access.
 */
public class ShizukuManager {
    private static final String TAG = "ESP-Shizuku";
    private static final int SHIZUKU_REQUEST_CODE = 100;

    private final Context context;
    private IShizukuService service;
    private boolean bound = false;

    private final Shizuku.OnRequestPermissionResultListener permissionListener =
        (requestCode, grantResult) -> {
            if (requestCode == SHIZUKU_REQUEST_CODE) {
                Log.i(TAG, "Permission result: " + (grantResult == PackageManager.PERMISSION_GRANTED));
            }
        };

    private final Shizuku.OnBinderReceivedListener binderReceivedListener =
        () -> Log.i(TAG, "Shizuku binder received");

    private final Shizuku.OnBinderDeadListener binderDeadListener =
        () -> {
            Log.w(TAG, "Shizuku binder dead");
            service = null;
            bound = false;
        };

    private final ServiceConnection serviceConnection = new ServiceConnection() {
        @Override
        public void onServiceConnected(ComponentName name, IBinder binder) {
            service = IShizukuService.Stub.asInterface(binder);
            bound = true;
            Log.i(TAG, "Shizuku service connected");
        }

        @Override
        public void onServiceDisconnected(ComponentName name) {
            service = null;
            bound = false;
            Log.w(TAG, "Shizuku service disconnected");
        }
    };

    public ShizukuManager(Context context) {
        this.context = context;
    }

    public boolean isAvailable() {
        try {
            return Shizuku.pingBinder();
        } catch (Exception e) {
            return false;
        }
    }

    public boolean hasPermission() {
        if (!isAvailable()) return false;
        return Shizuku.checkSelfPermission() == PackageManager.PERMISSION_GRANTED;
    }

    public void requestPermission() {
        if (!isAvailable()) return;
        if (!hasPermission()) {
            Shizuku.requestPermission(SHIZUKU_REQUEST_CODE);
        }
    }

    public void addListeners() {
        Shizuku.addRequestPermissionResultListener(permissionListener);
        Shizuku.addBinderReceivedListener(binderReceivedListener);
        Shizuku.addBinderDeadListener(binderDeadListener);
    }

    public void removeListeners() {
        Shizuku.removeRequestPermissionResultListener(permissionListener);
        Shizuku.removeBinderReceivedListener(binderReceivedListener);
        Shizuku.removeBinderDeadListener(binderDeadListener);
    }

    public boolean bind() {
        if (!hasPermission()) {
            Log.e(TAG, "No Shizuku permission");
            return false;
        }

        try {
            Shizuku.UserServiceArgs args = new Shizuku.UserServiceArgs(
                new ComponentName(context.getPackageName(), ShizukuService.class.getName()))
                .daemon(false)
                .processNameSuffix("esp_service")
                .debuggable(true)
                .version(1);

            Shizuku.bindUserService(args, serviceConnection);
            Log.i(TAG, "Shizuku bindUserService called, waiting for connection...");
            return true;
        } catch (Exception e) {
            Log.e(TAG, "Failed to bind Shizuku service", e);
            return false;
        }
    }

    public void unbind() {
        service = null;
        bound = false;
    }

    public byte[] readMemory(int pid, long address, int size) {
        if (service == null) return null;
        try {
            return service.readMemory(pid, address, size);
        } catch (RemoteException e) {
            Log.e(TAG, "readMemory failed", e);
            return null;
        }
    }

    public int findPid(String packageName) {
        if (service == null) return -1;
        try {
            return service.findPid(packageName);
        } catch (RemoteException e) {
            Log.e(TAG, "findPid failed", e);
            return -1;
        }
    }

    public long getModuleBase(int pid, String moduleName) {
        if (service == null) return 0;
        try {
            return service.getModuleBase(pid, moduleName);
        } catch (RemoteException e) {
            Log.e(TAG, "getModuleBase failed", e);
            return 0;
        }
    }

    public String[] listModules(int pid) {
        if (service == null) return new String[0];
        try {
            return service.listModules(pid);
        } catch (RemoteException e) {
            Log.e(TAG, "listModules failed", e);
            return new String[]{"ERROR: " + e.getMessage()};
        }
    }

    public String[] searchMaps(int pid, String keyword) {
        if (service == null) return new String[0];
        try {
            return service.searchMaps(pid, keyword);
        } catch (RemoteException e) {
            Log.e(TAG, "searchMaps failed", e);
            return new String[]{"ERROR: " + e.getMessage()};
        }
    }

    public long findModuleBaseNative(int pid, String moduleName) {
        if (service == null) return 0;
        try {
            return service.findModuleBaseNative(pid, moduleName);
        } catch (android.os.DeadObjectException e) {
            Log.e(TAG, "findModuleBaseNative: Shizuku process died", e);
            return -3;
        } catch (RemoteException e) {
            Log.e(TAG, "findModuleBaseNative failed", e);
            return -1;
        }
    }

    public String diagnosePid(int pid) {
        if (service == null) return "service not bound";
        try {
            return service.diagnosePid(pid);
        } catch (RemoteException e) {
            Log.e(TAG, "diagnosePid failed", e);
            return "ERROR: " + e.getMessage();
        }
    }

    public boolean isBound() {
        return bound && service != null;
    }
}
