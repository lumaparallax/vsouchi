/*
 * Copyright (C) 2026 Luma Parallax
 * SPDX-License-Identifier: GPL-3.0-only
 */

package io.github.lumaparallax.vsouchi.v4l2;

import android.view.Surface;

public class NativeBridge {
    static { System.loadLibrary("vsouchi_bridge"); }

    public native void nativeClearCameras();
    public native void nativeAddCamera(int index, String deviceName, String cameraId,
                                      int[] formats, int[] resFormats, int[] resWs, int[] resHs, int[] resFps,
                                      int cfa, boolean hasFocus, float minFocus, boolean hasExposure,
                                      long minExposure, long maxExposure, int orientation, int hflip);
    public native boolean nativeConnect(int udsFd);
    public native void nativeStartLoop(int socketFd);

    public native Surface nativeCreateImageReader(int width, int height, int format, int maxImages);
    public native void nativeDestroyImageReader();

    private CameraEngine cameraEngine;

    public void setCameraEngine(CameraEngine engine) {
        this.cameraEngine = engine;
    }

    public void onSetControl(int controlId, int value) {
        if (cameraEngine != null) {
            cameraEngine.applyControl(controlId, value);
        }
    }

    /* Dispatched by Bridge C-core when userspace calls VIDIOC_S_INPUT */
    public void onSelectInput(int index) {
        if (cameraEngine != null) {
            cameraEngine.switchCamera(index);
        }
    }

    public void onStreamOn(int width, int height, int fps, int format) {
        if (cameraEngine != null) {
            cameraEngine.start(width, height, fps, format);
        }
    }

    public void onStreamOff() {
        if (cameraEngine != null) {
            cameraEngine.stopStream();
        }
    }

    public void onDisconnect() {
        if (cameraEngine != null) {
            cameraEngine.destroy();
        }
    }
}
