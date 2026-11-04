/*
 * Copyright (C) 2026 Luma Parallax
 * Adapted from Termux:X11 (com.termux.x11.CmdEntryPoint)
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */

// Shell daemon entry point bridging CLI to Android application
package io.github.lumaparallax.vsouchi;

import android.annotation.SuppressLint;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import androidx.annotation.Keep;
import java.net.URL;

@Keep
@SuppressLint({"StaticFieldLeak", "UnsafeDynamicallyLoadedCode"})
public class DaemonEntry extends IBridgeBinder.Stub {

    private static final String TAG = "vSouchi:Daemon";
    static final Handler handler;
    private static volatile DaemonEntry sInstance;

    static {
        try { if (Looper.getMainLooper() == null) Looper.prepareMainLooper(); } catch (Exception ignored) {}
        handler = new Handler();
        
        String libName = "libvsouchi_broker.so";
        String path = "lib/" + android.os.Build.SUPPORTED_ABIS[0] + "/" + libName;
        try {
            URL res = DaemonEntry.class.getClassLoader().getResource(path);
            String libPath = res != null ? res.getFile().replace("file:", "") : null;
            if (libPath != null) System.load(libPath);
            else throw new RuntimeException("Failed to locate " + libName);
        } catch (Exception e) {
            Logger.e(TAG, "Native library load failed", e);
            System.exit(134);
        }
    }

    public static void main(String[] args) {
        Logger.i(TAG, "Starting vSouchi Shell Daemon...");
        
        // Kills Java process if the shell pipe closes, ensuring no orphaned daemons
        new Thread(() -> {
            try {
                int read = System.in.read();
                Logger.i(TAG, "Stdin closed. Exiting...");
            } catch (Exception e) {
                Logger.e(TAG, "Stdin error", e);
            } finally {
                System.exit(0);
            }
        }, "ShellWatchdog").start();

        handler.post(() -> sInstance = new DaemonEntry());
        Looper.loop();
    }

    public DaemonEntry() {
        sInstance = this;
        new Thread(DaemonEntry::start, "vsouchi-broker").start();
        ActivityLauncher.wakeApp(this);
    }

    @Keep
    public static void requestWakeApp() {
        if (sInstance != null) {
            Logger.i(TAG, "Broker triggered on-demand APK re-wake.");
            handler.post(() -> ActivityLauncher.wakeApp(sInstance));
        }
    }

    @Override
    public ParcelFileDescriptor socketPairFD() {
        int fd = nativeGetUds();
        if (fd >= 0) {
            Logger.i(TAG, "Acquired UNIX Socket FD: " + fd + ". Handing to Android APK.");
            return ParcelFileDescriptor.adoptFd(fd);
        }
        return null;
    }

    public static native void start();
    private static native int nativeGetUds();
    public static native void stop();
}
