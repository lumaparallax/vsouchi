/*
 * Copyright (C) 2026 Luma Parallax
 * SPDX-License-Identifier: GPL-3.0-only
 */

package io.github.lumaparallax.vsouchi.v4l2;

import android.hardware.camera2.CameraCaptureSession;
import android.hardware.camera2.CameraDevice;
import android.hardware.camera2.CaptureRequest;
import android.os.Handler;
import android.util.Range;
import android.view.Surface;
import io.github.lumaparallax.vsouchi.Logger;

public class CaptureSessionController {
    private static final String TAG = "vSouchi:V4L2Ctrl";

    private static final int V4L2_CID_EXPOSURE_AUTO = 0x009a0901;
    private static final int V4L2_CID_EXPOSURE_ABSOLUTE = 0x009a0902;
    private static final int V4L2_CID_FOCUS_ABSOLUTE = 0x009a090a;
    private static final int V4L2_CID_FOCUS_AUTO = 0x009a090c;

    private int stateFocusMode = CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO;
    private float stateFocusDistance = 0.0f;
    private int stateAeMode = CaptureRequest.CONTROL_AE_MODE_ON;
    private long stateExposureTime = 15600000L; 

    public void applyControl(int controlId, int value, CameraDevice device, Surface activeSurface, int activeFps, CameraCaptureSession session, Handler handler) {
        switch (controlId) {
            case V4L2_CID_FOCUS_AUTO:
                stateFocusMode = (value == 1) ? CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO : CaptureRequest.CONTROL_AF_MODE_OFF;
                break;
            case V4L2_CID_FOCUS_ABSOLUTE:
                // Android uses Diopters (0.0f is infinity, max is macro) instead of V4L2's 0-100 scale.
                float maxDiopters = 10.0f; 
                stateFocusDistance = (value / 100.0f) * maxDiopters;
                break;
            case V4L2_CID_EXPOSURE_AUTO:
                stateAeMode = (value == 1) ? CaptureRequest.CONTROL_AE_MODE_OFF : CaptureRequest.CONTROL_AE_MODE_ON;
                break;
            case V4L2_CID_EXPOSURE_ABSOLUTE:
                // Map V4L2's 1-5000 standardized slider roughly to Android nanoseconds
                stateExposureTime = (long) value * 100000L;
                break;
        }

        if (session != null && device != null && activeSurface != null) {
            updateRepeatingRequest(device, activeSurface, activeFps, session, handler);
        }
    }

    public void updateRepeatingRequest(CameraDevice cameraDevice, Surface activeSurface, int activeFps, CameraCaptureSession captureSession, Handler backgroundHandler) {
        try {
            CaptureRequest.Builder builder = cameraDevice.createCaptureRequest(CameraDevice.TEMPLATE_RECORD);
            builder.addTarget(activeSurface);
            builder.set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, new Range<>(activeFps, activeFps));

            builder.set(CaptureRequest.CONTROL_AF_MODE, stateFocusMode);
            if (stateFocusMode == CaptureRequest.CONTROL_AF_MODE_OFF) {
                builder.set(CaptureRequest.LENS_FOCUS_DISTANCE, stateFocusDistance);
            }

            builder.set(CaptureRequest.CONTROL_AE_MODE, stateAeMode);
            if (stateAeMode == CaptureRequest.CONTROL_AE_MODE_OFF) {
                builder.set(CaptureRequest.SENSOR_EXPOSURE_TIME, stateExposureTime);
                // Lock ISO to prevent flicker when forcing manual exposure
                builder.set(CaptureRequest.SENSOR_SENSITIVITY, 200); 
            }

            captureSession.setRepeatingRequest(builder.build(), null, backgroundHandler);
        } catch (Exception e) { 
            Logger.e(TAG, "Failed to update repeating request", e); 
        }
    }
}
