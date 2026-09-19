/*
 * Copyright (C) 2026 Luma Parallax
 * SPDX-License-Identifier: GPL-3.0-only
 */

package io.github.lumaparallax.vsouchi;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Intent;
import android.os.Build;
import android.os.IBinder;
import io.github.lumaparallax.vsouchi.v4l2.CameraEngine;
import io.github.lumaparallax.vsouchi.v4l2.CameraHardwareManager;
import io.github.lumaparallax.vsouchi.v4l2.NativeBridge;

public class ForegroundService extends Service {
    private static final String TAG = "vSouchi:Service";
    private NativeBridge nativeBridge;
    private CameraEngine cameraEngine;
    private CameraHardwareManager hardwareManager;

    @Override
    public void onCreate() {
        super.onCreate();
        
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            NotificationChannel channel = new NotificationChannel("vsouchi_chan", "vSouchi IPC", NotificationManager.IMPORTANCE_LOW);
            NotificationManager manager = getSystemService(NotificationManager.class);
            if (manager != null) manager.createNotificationChannel(channel);
            
            Notification notification = new Notification.Builder(this, "vsouchi_chan")
                    .setContentTitle("vSouchi Active")
                    .setContentText("Background IPC running")
                    .setSmallIcon(android.R.drawable.ic_menu_info_details)
                    .build();
            try {
                if (Build.VERSION.SDK_INT >= 29) { 
                    startForeground(4040, notification, android.content.pm.ServiceInfo.FOREGROUND_SERVICE_TYPE_CAMERA);
                } else {
                    startForeground(4040, notification);
                }
            } catch (Exception e) {
                Logger.e(TAG, "startForeground denied. Running as background service.", e);
            }
        }
        
        nativeBridge = new NativeBridge();
        cameraEngine = new CameraEngine(this, nativeBridge);
        nativeBridge.setCameraEngine(cameraEngine);
        
        hardwareManager = new CameraHardwareManager(this, nativeBridge);
        hardwareManager.probeAndPushCapabilities();
        
        Logger.i(TAG, "Bridge Service created.");
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent != null) {
            int socketFd = intent.getIntExtra("socket_fd", -1);
            if (socketFd != -1) {
                if (nativeBridge.nativeConnect(socketFd)) {
                    
                    // Wrap the native loop so it blocks until the shell environment disconnects.
                    new Thread(() -> {
                        nativeBridge.nativeStartLoop(socketFd); 
                        
                        Logger.i(TAG, "IPC Loop exited. Shutting down physical camera lens and service.");
                        
                        // Physically kill the camera lens
                        if (cameraEngine != null) {
                            cameraEngine.destroy();
                        }
                        
                        if (Build.VERSION.SDK_INT >= 24) {
                            stopForeground(Service.STOP_FOREGROUND_REMOVE);
                        } else {
                            stopForeground(true);
                        }
                        
                        // Kill the service so it doesn't run infinitely in the background after disconnect.
                        stopSelf();
                        
                    }, "NativeBridgeLoop").start();
                    
                }
            }
        }
        // Tell OS to recreate if killed, though the daemon will usually drive this.
        return START_STICKY; 
    }

    @Override
    public void onDestroy() {
        super.onDestroy();
        if (cameraEngine != null) {
            cameraEngine.destroy();
        }
        Logger.i(TAG, "Bridge Service destroyed.");
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }
}
