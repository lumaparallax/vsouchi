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

public class MainActivity extends Activity implements PermissionsHelper.PermissionCallback {
    private static final String TAG = "vSouchi:Main";
    private Intent pendingServiceIntent;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        handleIntent();
    }

    private void handleIntent() {
        if (getIntent() != null && getIntent().hasExtra("ipc_bundle")) {
            Logger.i(TAG, "Routing Intent from shell environment to ForegroundService...");
            Bundle b = getIntent().getBundleExtra("ipc_bundle");

            if (b != null) {
                IBinder binder = b.getBinder("ipc_binder");
                if (binder != null) {
                    try {
                        IBridgeBinder bridgeBinder = IBridgeBinder.Stub.asInterface(binder);
                        ParcelFileDescriptor pfd = bridgeBinder.socketPairFD();
                        
                        if (pfd != null) {
                            // Detach FD immediately so it survives Activity destruction and garbage collection.
                            int apkSocketFd = pfd.detachFd(); 
                            Logger.i(TAG, "Extracted Socket FD: " + apkSocketFd + ". Checking permissions...");

                            pendingServiceIntent = new Intent(this, ForegroundService.class);
                            pendingServiceIntent.putExtra("socket_fd", apkSocketFd);
                            
                            PermissionsHelper.checkAndRequest(this, this);
                            return; 
                        }
                    } catch (Exception e) {
                        Logger.e(TAG, "Binder transaction failed", e);
                    }
                }
            }
            finish();
            
        } else {
            TextView tv = new TextView(this);
            tv.setText("vSouchi IPC Framework\nLaunch via shell environment.");
            setContentView(tv);
        }
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        PermissionsHelper.handleResult(requestCode, grantResults, this);
    }

    @Override
    public void onPermissionsGranted() {
        if (pendingServiceIntent != null) {
            try {
                // startForegroundService grants a legal 5-second grace period for the background service 
                // to post its notification even after this Activity calls finish() and dies.
                if (Build.VERSION.SDK_INT >= 26) {
                    startForegroundService(pendingServiceIntent);
                } else {
                    startService(pendingServiceIntent);
                }
            } catch (Exception e) {
                Logger.e(TAG, "Failed to start ForegroundService", e);
            }
        }
        moveTaskToBack(true);
    }

    @Override
    public void onPermissionsDenied() {
        Toast.makeText(this, "Camera and Notification permissions are required!", Toast.LENGTH_LONG).show();
        finish(); 
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        handleIntent();
    }
}
