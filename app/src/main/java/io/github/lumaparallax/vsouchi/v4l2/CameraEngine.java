/*
 * Copyright (C) 2026 Luma Parallax
 * SPDX-License-Identifier: GPL-3.0-only
 */

package io.github.lumaparallax.vsouchi.v4l2;

import android.annotation.SuppressLint;
import android.content.Context;
import android.graphics.ImageFormat;
import android.hardware.camera2.*;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Range;
import android.view.Surface;
import java.util.Arrays;
import io.github.lumaparallax.vsouchi.Logger;

public class CameraEngine {
    private static final String TAG = "vSouchi:Camera";
    private final Context context;
    private final NativeBridge bridge;
    private final CaptureSessionController sessionController;
    
    private CameraDevice cameraDevice;
    private CameraCaptureSession captureSession;
    private Surface activeSurface; 
    
    private HandlerThread backgroundThread;
    private Handler backgroundHandler;
    
    private volatile boolean isStreaming = false;
    private volatile boolean isCameraBooting = false;
    private int activeWidth = 0;
    private int activeHeight = 0;
    private int activeFps = 30;
    private int activeFormat = ImageFormat.YUV_420_888;

    public CameraEngine(Context context, NativeBridge bridge) {
        this.context = context;
        this.bridge = bridge;
        this.sessionController = new CaptureSessionController();
    }

    @SuppressLint("MissingPermission")
    public void start(int width, int height, int fps, int format) {
        if (isStreaming && width == activeWidth && height == activeHeight && fps == activeFps && format == activeFormat) return;
        isStreaming = false; 

        if (backgroundThread == null) {
            backgroundThread = new HandlerThread("CameraThread");
            backgroundThread.start();
            backgroundHandler = new Handler(backgroundThread.getLooper());
        }

        boolean needsNewReader = (activeSurface == null || width != activeWidth || height != activeHeight || format != activeFormat);

        int template = (format == ImageFormat.RAW_SENSOR)
                ? CameraDevice.TEMPLATE_STILL_CAPTURE
                : CameraDevice.TEMPLATE_RECORD;
        sessionController.setTemplateType(template);

        if (needsNewReader) {
            bridge.nativeDestroyImageReader();
            activeSurface = bridge.nativeCreateImageReader(width, height, format, 3);
            
            if (activeSurface == null) {
                Logger.e(TAG, "FATAL: C-Bridge failed to allocate NDK ImageReader!", null);
                return;
            }

            activeWidth = width;
            activeHeight = height;
            activeFps = fps;
            activeFormat = format;

            if (cameraDevice != null) {
                if (captureSession != null) {
                    try { 
                        captureSession.stopRepeating();
                        captureSession.abortCaptures(); 
                        captureSession.close(); 
                    } catch (Exception ignored) {}
                    captureSession = null;
                }
                createSession(); 
            }

        } else if (cameraDevice != null && captureSession != null) {
            activeFps = fps; 
            sessionController.updateRepeatingRequest(cameraDevice, activeSurface, activeFps, captureSession, backgroundHandler);
        }

        if (cameraDevice == null) {
            if (isCameraBooting) return; 
            isCameraBooting = true;
            try {
                CameraManager manager = (CameraManager) context.getSystemService(Context.CAMERA_SERVICE);
                String cid = manager.getCameraIdList()[0];
                
                manager.openCamera(cid, new CameraDevice.StateCallback() {
                    @Override
                    public void onOpened(CameraDevice camera) {
                        isCameraBooting = false; 
                        cameraDevice = camera;
                        createSession(); 
                    }
                    @Override public void onDisconnected(CameraDevice c) { 
                        isCameraBooting = false; 
                        c.close(); cameraDevice = null; 
                    }
                    @Override public void onError(CameraDevice c, int e) { 
                        isCameraBooting = false; 
                        c.close(); cameraDevice = null; 
                    }
                }, backgroundHandler);
            } catch (Exception e) { isCameraBooting = false; }
        }
        
        isStreaming = true; 
    }

    private void createSession() {
        if (activeSurface == null || cameraDevice == null) return;

        try {
            int template = (activeFormat == ImageFormat.RAW_SENSOR)
                    ? CameraDevice.TEMPLATE_STILL_CAPTURE
                    : CameraDevice.TEMPLATE_RECORD;
            final CaptureRequest.Builder builder = cameraDevice.createCaptureRequest(template);
            builder.addTarget(activeSurface);

            // Zero latency hardware fix: forces Android to strictly respect the V4L2 requested framerate 
            // Didnt work as expected but acceptable for now.
            builder.set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, new Range<>(activeFps, activeFps));

            cameraDevice.createCaptureSession(Arrays.asList(activeSurface), new CameraCaptureSession.StateCallback() {
                @Override
                public void onConfigured(CameraCaptureSession session) {
                    captureSession = session;
                    sessionController.updateRepeatingRequest(cameraDevice, activeSurface, activeFps, captureSession, backgroundHandler);
                }
                @Override public void onConfigureFailed(CameraCaptureSession s) {
                    Logger.e(TAG, "Hardware REJECTED the session!", null);
                }
            }, backgroundHandler);
        } catch (Exception e) { 
            Logger.e(TAG, "Session creation failed", e); 
        }
    }

    public void applyControl(int controlId, int value) {
        if (isStreaming) {
            sessionController.applyControl(controlId, value, cameraDevice, activeSurface, activeFps, captureSession, backgroundHandler);
        }
    }

    public void stopStream() {
        isStreaming = false;
        if (captureSession != null) {
            try {
                captureSession.stopRepeating();
                captureSession.abortCaptures(); 
            } catch (Exception ignored) {}
        }
    }

    public void destroy() {
        isStreaming = false;

        if (captureSession != null) { 
            try { captureSession.stopRepeating(); captureSession.abortCaptures(); } catch (Exception ignored) {}
            try { captureSession.close(); } catch (Exception ignored) {}
            captureSession = null; 
        }

        if (cameraDevice != null) { cameraDevice.close(); cameraDevice = null; }
        
        bridge.nativeDestroyImageReader();
        activeSurface = null;
        
        if (backgroundThread != null) {
            backgroundThread.quitSafely();
            try { backgroundThread.join(500); } catch (Exception ignored) {}
            backgroundThread = null; backgroundHandler = null;
        }

        activeWidth = 0; activeHeight = 0; isCameraBooting = false;
    }
}
