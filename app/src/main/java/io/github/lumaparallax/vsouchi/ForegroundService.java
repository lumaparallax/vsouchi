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
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.IBinder;
import androidx.core.content.ContextCompat;
import io.github.lumaparallax.vsouchi.v4l2.CameraEngine;
import io.github.lumaparallax.vsouchi.v4l2.CameraHardwareManager;
import io.github.lumaparallax.vsouchi.v4l2.NativeBridge;

public class ForegroundService extends Service {
    private static final String TAG = "vSouchi:Service";
    private static final String CHANNEL_ID = "vsouchi_ipc_channel";
    private static final int NOTIFICATION_ID = 4040;
    private NativeBridge nativeBridge;
    private CameraEngine cameraEngine;
    private CameraHardwareManager hardwareManager; 

    @Override
    public void onCreate() {
        super.onCreate();
        
        NotificationChannel channel = new NotificationChannel(
                CHANNEL_ID,
                "vSouchi IPC Service",
                NotificationManager.IMPORTANCE_LOW
        );
        channel.setDescription("Maintains background V4L2 device streaming and IPC routing");
        
        NotificationManager manager = getSystemService(NotificationManager.class);
        if (manager != null) manager.createNotificationChannel(channel);
        
        int badgeColor = ContextCompat.getColor(this, R.color.notification_background);

        Notification notification = new Notification.Builder(this, CHANNEL_ID)
                .setContentTitle("vSouchi Active")
                .setContentText("V4L2 virtualization pipeline running")
                .setSmallIcon(R.drawable.ic_notification)
                .setColor(badgeColor)
                .setOngoing(true)
                .setCategory(Notification.CATEGORY_SERVICE)
                .setShowWhen(false)
                .build();

        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) { 
                startForeground(NOTIFICATION_ID, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_CAMERA);
            } else {
                startForeground(NOTIFICATION_ID, notification);
            }
        } catch (Exception e) {
            Logger.e(TAG, "Fatal: startForeground denied", e);
            stopSelf();
            return;
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
