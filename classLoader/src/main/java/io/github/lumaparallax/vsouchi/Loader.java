/*
 * Copyright (C) 2026 Luma Parallax
 * Adapted from Termux:X11 (com.termux.x11.Loader)
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */

package io.github.lumaparallax.vsouchi;

import io.github.lumaparallax.vsouchi.classLoader.BuildConfig;
import java.security.MessageDigest;

/**
 * Command-line entry point executed via app_process.
 *
 * 1. Checks if the vSouchi app is installed.
 * 2. Verifies the APK SHA-256 certificate signature against BuildConfig.
 * 3. Loads the app code with PathClassLoader and runs DaemonEntry.main().
 */
public class Loader {
    public static void main(String[] args) {
        try {
            android.content.pm.PackageInfo targetInfo = (android.os.Build.VERSION.SDK_INT <= 32) ?
                    android.app.ActivityThread.getPackageManager().getPackageInfo(BuildConfig.APPLICATION_ID, android.content.pm.PackageManager.GET_SIGNATURES, 0) :
                    android.app.ActivityThread.getPackageManager().getPackageInfo(BuildConfig.APPLICATION_ID, (long) android.content.pm.PackageManager.GET_SIGNATURES, 0);

            if (targetInfo == null)
                throw new AssertionError(BuildConfig.packageNotInstalledErrorText.replace("ARCH", android.os.Build.SUPPORTED_ABIS[0]));

            if (targetInfo.signatures == null || targetInfo.signatures.length != 1)
                throw new AssertionError(BuildConfig.packageSignatureMismatchErrorText);

            MessageDigest md = MessageDigest.getInstance("SHA-256");
            byte[] digest = md.digest(targetInfo.signatures[0].toByteArray());
            StringBuilder sb = new StringBuilder();
            for (byte b : digest) {
                sb.append(String.format("%02x", b));
            }

            if (!BuildConfig.SIGNATURE_SHA256.equalsIgnoreCase(sb.toString()))
                throw new AssertionError(BuildConfig.packageSignatureMismatchErrorText);

            android.util.Log.i(BuildConfig.logTag, "loading " + targetInfo.applicationInfo.sourceDir + "::" + BuildConfig.CLASS_ID + "::main of " + BuildConfig.APPLICATION_ID + " application");
            Class<?> targetClass = Class.forName(BuildConfig.CLASS_ID, true,
                    new dalvik.system.PathClassLoader(targetInfo.applicationInfo.sourceDir, null, ClassLoader.getSystemClassLoader()));
            targetClass.getMethod("main", String[].class).invoke(null, (Object) args);
        } catch (AssertionError e) {
            System.err.println(e.getMessage());
        } catch (java.lang.reflect.InvocationTargetException e) {
            e.getCause().printStackTrace(System.err);
        } catch (Throwable e) {
            android.util.Log.e(BuildConfig.logTag, "Loader error", e);
            e.printStackTrace(System.err);
        }
    }
}
