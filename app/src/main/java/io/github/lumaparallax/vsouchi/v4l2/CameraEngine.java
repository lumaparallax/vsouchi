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
    private int targetCameraIndex = 0;

    public CameraEngine(Context context, NativeBridge bridge) {
        this.context = context;
        this.bridge = bridge;
        this.sessionController = new CaptureSessionController();
    }

    public void setMaxFocusDiopters(float maxDiopters) {
        sessionController.setMaxFocusDiopters(maxDiopters);
    }

    public synchronized void switchCamera(int index) {
        if (targetCameraIndex == index) return;
        targetCameraIndex = index;

        /* Enforce sequential teardown before booting the next lens to avoid ISP lockups */
        if (captureSession != null) {
            try { captureSession.stopRepeating(); captureSession.abortCaptures(); } catch (Exception ignored) {}
            try { captureSession.close(); } catch (Exception ignored) {}
            captureSession = null;
        }
        if (cameraDevice != null) {
            cameraDevice.close();
            cameraDevice = null;
        }

        bridge.nativeDestroyImageReader();
        activeSurface = null;
        activeWidth = 0;
        activeHeight = 0;
        isCameraBooting = false;
    }

    @SuppressLint("MissingPermission")
    public synchronized void start(int width, int height, int fps, int format) {
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
            activeSurface = bridge.nativeCreateImageReader(width, height, format, 4);

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
                String[] ids = manager.getCameraIdList();
                String cid = (targetCameraIndex >= 0 && targetCameraIndex < ids.length)
                        ? ids[targetCameraIndex]
                        : ids[0];

                try {
                    CameraCharacteristics chars = manager.getCameraCharacteristics(cid);
                    Float minFocus = chars.get(CameraCharacteristics.LENS_INFO_MINIMUM_FOCUS_DISTANCE);
                    if (minFocus != null && minFocus > 0.0f) {
                        setMaxFocusDiopters(minFocus);
                    }
                    Range<Integer>[] ranges = chars.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES);
                    sessionController.setAvailableFpsRanges(ranges);
                } catch (Exception ignored) {}

                manager.openCamera(cid, new CameraDevice.StateCallback() {
                    @Override
                    public void onOpened(CameraDevice camera) {
                        synchronized (CameraEngine.this) {
                            isCameraBooting = false;
                            /* Abandon configuration if stream was stopped before hardware finished opening */
                            if (!isStreaming) {
                                camera.close();
                                return;
                            }
                            cameraDevice = camera;
                            createSession();
                        }
                    }
                    @Override public void onDisconnected(CameraDevice c) {
                        synchronized (CameraEngine.this) {
                            isCameraBooting = false;
                            c.close(); cameraDevice = null;
                        }
                    }
                    @Override public void onError(CameraDevice c, int e) {
                        synchronized (CameraEngine.this) {
                            isCameraBooting = false;
                            c.close(); cameraDevice = null;
                        }
                    }
                }, backgroundHandler);
            } catch (Exception e) { isCameraBooting = false; }
        }

        isStreaming = true;
    }

    private synchronized void createSession() {
        if (activeSurface == null || cameraDevice == null) return;

        try {
            cameraDevice.createCaptureSession(Arrays.asList(activeSurface), new CameraCaptureSession.StateCallback() {
                @Override
                public void onConfigured(CameraCaptureSession session) {
                    synchronized (CameraEngine.this) {
                        /* Do not start repeating requests if stream was stopped during session creation */
                        if (!isStreaming || cameraDevice == null) {
                            session.close();
                            return;
                        }
                        captureSession = session;
                        sessionController.updateRepeatingRequest(cameraDevice, activeSurface, activeFps, captureSession, backgroundHandler);
                    }
                }
                @Override public void onConfigureFailed(CameraCaptureSession s) {
                    Logger.e(TAG, "Hardware REJECTED the session!", null);
                }
            }, backgroundHandler);
        } catch (Exception e) {
            Logger.e(TAG, "Session creation failed", e);
        }
    }

    public synchronized void applyControl(int controlId, int value) {
        if (isStreaming) {
            sessionController.applyControl(controlId, value, cameraDevice, activeSurface, activeFps, captureSession, backgroundHandler);
        }
    }

    public synchronized void stopStream() {
        isStreaming = false;
        if (captureSession != null) {
            try {
                captureSession.stopRepeating();
                captureSession.abortCaptures();
            } catch (Exception ignored) {}
        }
    }

    public synchronized void destroy() {
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
