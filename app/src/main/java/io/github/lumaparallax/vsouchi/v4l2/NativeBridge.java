/*
 * Copyright (C) 2026 Luma Parallax
 * SPDX-License-Identifier: GPL-3.0-only
 */

package io.github.lumaparallax.vsouchi.v4l2;

import android.view.Surface;

public class NativeBridge {
    static { System.loadLibrary("vsouchi_bridge"); }

    public native void nativeSetDeviceInfo(String deviceName);
    public native void nativeSetResolutions(int[] formats, int[] widths, int[] heights, int[] fps);
    public native void nativeSetFormats(int[] formats);
    public native boolean nativeConnect(int udsFd);
    public native void nativeStartLoop(int socketFd);

    public native Surface nativeCreateImageReader(int width, int height, int format, int maxImages);
    public native void nativeDestroyImageReader();

    private CameraEngine cameraEngine;

    public void setCameraEngine(CameraEngine engine) {
        this.cameraEngine = engine;
    }

    public native void nativeSetCameraCaps(boolean hasFocus, float minFocus, boolean hasExposure, long minExposure, long maxExposure, int orientation, int hflip);

    public void onSetControl(int controlId, int value) {
        if (cameraEngine != null) {
            cameraEngine.applyControl(controlId, value);
        }
    }

    public void onStreamOn(int width, int height, int fps, int format) {
        if (cameraEngine != null) {
            cameraEngine.start(width, height, fps, format);
        }
    }

    public void onStreamOff() {
        if (cameraEngine != null) {
            new Thread(() -> {
                cameraEngine.stopStream(); 
            }).start();
        }
    }

    public void onDisconnect() {
        if (cameraEngine != null) {
            new Thread(() -> {
                cameraEngine.destroy(); 
            }).start();
        }
    }
}
