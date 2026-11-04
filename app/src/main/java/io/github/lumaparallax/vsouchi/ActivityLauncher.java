/*
 * Copyright (C) 2026 Luma Parallax
 * Adapted from Termux:X11 (com.termux.x11.CmdEntryPoint)
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */

package io.github.lumaparallax.vsouchi;

import static android.system.Os.getuid;
import android.annotation.SuppressLint;
import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.os.IBinder;

public class ActivityLauncher {
    private static final String TAG = "vSouchi:Launcher";
    private static long lastWakeTimestamp = 0;
    private static final Context ctx = createContext();

    public static synchronized void wakeApp(IBinder binder) {
        long now = System.currentTimeMillis();
        if (now - lastWakeTimestamp < 1000) {
            Logger.i(TAG, "Wake Intent throttled (already sent <1s ago).");
            return;
        }
        lastWakeTimestamp = now;

        Logger.i(TAG, "Firing Intent to wake Android App...");
        Intent intent = createIntent(binder);
        try {
            if (ctx != null) ctx.startActivity(intent);
            else throw new RuntimeException("Null context");
        } catch (Exception e) {
            Logger.e(TAG, "Standard Launch Failed. Trying Reflection fallback...", e);
            startActivityManual(intent);
        }
    }

    private static Intent createIntent(IBinder binder) {
        String targetPkg = BuildConfig.APPLICATION_ID;
        String targetActivity = "io.github.lumaparallax.vsouchi.MainActivity";

        Bundle b = new Bundle();
        b.putBinder("ipc_binder", binder);

        Intent i = new Intent();
        i.setClassName(targetPkg, targetActivity);
        i.putExtra("ipc_bundle", b);
        i.setFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_INCLUDE_STOPPED_PACKAGES);
        return i;
    }


    private static void startActivityManual(Intent intent) {
        try {
            // Dynamically fetch the package name of the shell environment host
            String packageName = "android"; 
            try {
                 String[] pkgs = android.app.ActivityThread.getPackageManager().getPackagesForUid(getuid());
                 if (pkgs != null && pkgs.length > 0) packageName = pkgs[0];
            } catch (Exception ignore) {}

            Object am;
            try {
                am = Class.forName("android.app.ActivityManager").getMethod("getService").invoke(null);
            } catch (Exception e) {
                am = Class.forName("android.app.ActivityManagerNative").getMethod("getDefault").invoke(null);
            }

            // Bypass background start restrictions by mimicking a PendingIntent sender
            int INTENT_SENDER_ACTIVITY = 2;
            Class<?> receiverClass = Class.forName("android.content.IIntentReceiver");
            java.lang.reflect.Method getSender = am.getClass().getMethod("getIntentSender",
                    int.class, String.class, IBinder.class, String.class, int.class, Intent[].class, String[].class, int.class, Bundle.class, int.class);
            Object sender = getSender.invoke(am,
                    INTENT_SENDER_ACTIVITY, packageName, null, null, 0, new Intent[] { intent }, null,
                    PendingIntent.FLAG_CANCEL_CURRENT | PendingIntent.FLAG_ONE_SHOT, null, 0);
            java.lang.reflect.Method send = sender.getClass().getMethod("send",
                    int.class, Intent.class, String.class, IBinder.class, receiverClass, String.class, Bundle.class);
            send.invoke(sender, 0, intent, null, null, null, null, null);
        } catch (Exception ex) {
            Logger.e(TAG, "Manual Launch Failed", ex);
        }
    }

    @SuppressLint("DiscouragedPrivateApi")
    private static Context createContext() {
        // Forge a system context using Unsafe to avoid needing an active Application instance
        try {
            var f = Class.forName("sun.misc.Unsafe").getDeclaredField("theUnsafe");
            f.setAccessible(true);
            Object unsafe = f.get(null);
            return ((android.app.ActivityThread) Class.forName("sun.misc.Unsafe").getMethod("allocateInstance", Class.class).invoke(unsafe, android.app.ActivityThread.class)).getSystemContext();
        } catch (Exception e) { return null; }
    }
}
