/*
 * Copyright (C) 2026 Luma Parallax
 * SPDX-License-Identifier: GPL-3.0-only
 */

package io.github.lumaparallax.vsouchi;

import android.Manifest;
import android.app.Activity;
import android.content.pm.PackageManager;
import android.os.Build;
import java.util.ArrayList;
import java.util.List;

public class PermissionsHelper {
    public static final int PERM_REQ_CODE = 1337;

    public interface PermissionCallback {
        void onPermissionsGranted();
        void onPermissionsDenied();
    }

    public static void checkAndRequest(Activity activity, PermissionCallback callback) {
        List<String> needed = new ArrayList<>();

        if (activity.checkSelfPermission(Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
            needed.add(Manifest.permission.CAMERA);
        }

        if (Build.VERSION.SDK_INT >= 33 && activity.checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) {
            needed.add(Manifest.permission.POST_NOTIFICATIONS);
        }

        if (needed.isEmpty()) {
            callback.onPermissionsGranted();
        } else {
            activity.requestPermissions(needed.toArray(new String[0]), PERM_REQ_CODE);
        }
    }

    public static void handleResult(int requestCode, int[] grantResults, PermissionCallback callback) {
        if (requestCode == PERM_REQ_CODE) {
            for (int res : grantResults) {
                if (res != PackageManager.PERMISSION_GRANTED) {
                    callback.onPermissionsDenied();
                    return;
                }
            }
            callback.onPermissionsGranted();
        }
    }
}
