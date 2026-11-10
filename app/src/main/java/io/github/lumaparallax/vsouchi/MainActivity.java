/*
 * Copyright (C) 2026 Luma Parallax
 * SPDX-License-Identifier: GPL-3.0-only
 */

package io.github.lumaparallax.vsouchi;

import android.app.Activity;
import android.content.Intent;
import android.os.Build;
import android.os.Bundle;
import android.os.IBinder;
import android.os.ParcelFileDescriptor;
import android.widget.TextView;
import android.widget.Toast;

import java.io.IOException;

public class MainActivity extends Activity implements PermissionsHelper.PermissionCallback {
    private static final String TAG = "vSouchi:Main";

    private Intent pendingServiceIntent;
    private int apkSocketFd = -1;
    private boolean isDaemonLaunch = false;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        handleIntent();
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        handleIntent();
    }

    private void handleIntent() {
        Intent intent = getIntent();
        if (intent != null && intent.hasExtra("ipc_bundle")) {
            isDaemonLaunch = true;
            Logger.i(TAG, "Routing Intent from shell environment to ForegroundService...");
            Bundle b = intent.getBundleExtra("ipc_bundle");

            if (b != null) {
                IBinder binder = b.getBinder("ipc_binder");
                if (binder instanceof NativeBridge.BridgeBinder) {
                    NativeBridge.BridgeBinder bridgeBinder = (NativeBridge.BridgeBinder) binder;
                    try {
                        ParcelFileDescriptor pfd = bridgeBinder.socketPairFD();

                        if (pfd != null) {
                            apkSocketFd = pfd.detachFd();

                            if (apkSocketFd < 0) {
                                Logger.e(TAG, "Failed to detach valid socket descriptor");
                                finishAndRemoveTask();
                                return;
                            }

                            Logger.i(TAG, "Extracted Socket FD: " + apkSocketFd + ". Checking permissions...");

                            pendingServiceIntent = new Intent(this, ForegroundService.class);
                            pendingServiceIntent.putExtra("socket_fd", apkSocketFd);

                            PermissionsHelper.checkAndRequestPermissions(this, this);
                            return;
                        }
                    } catch (Exception e) {
                        Logger.e(TAG, "Binder transaction failed", e);
                        closeSocketFd();
                    }
                }
            }
            finishAndRemoveTask();
        } else {
            isDaemonLaunch = false;
            TextView tv = new TextView(this);
            tv.setText("vSouchi IPC Framework\nLaunch via shell environment.");
            setContentView(tv);
        }
    }

    @Override
    public void onPermissionsGranted() {
        if (isDaemonLaunch) {
            if (pendingServiceIntent != null) {
                try {
                    if (Build.VERSION.SDK_INT >= 26) {
                        startForegroundService(pendingServiceIntent);
                    } else {
                        startService(pendingServiceIntent);
                    }
                } catch (Exception e) {
                    Logger.e(TAG, "Failed to start ForegroundService", e);
                    closeSocketFd();
                }
            }
            finishAndRemoveTask();
        }
    }

    @Override
    public void onPermissionsDenied() {
        if (isDaemonLaunch) {
            closeSocketFd();
            Toast.makeText(this, "Camera and Notification permissions are required!", Toast.LENGTH_LONG).show();
            finishAndRemoveTask();
        }
    }

    private void closeSocketFd() {
        if (apkSocketFd >= 0) {
            try {
                ParcelFileDescriptor.adoptFd(apkSocketFd).close();
            } catch (IOException e) {
                Logger.e(TAG, "Failed to close socket FD", e);
            } finally {
                apkSocketFd = -1;
            }
        }
    }
}
