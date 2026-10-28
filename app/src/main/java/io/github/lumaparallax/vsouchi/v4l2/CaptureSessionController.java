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
    private float maxFocusDiopters = 10.0f;

    private int templateType = CameraDevice.TEMPLATE_RECORD;
    private Range<Integer>[] availableFpsRanges;

    public void setAvailableFpsRanges(Range<Integer>[] ranges) {
        this.availableFpsRanges = ranges;
    }

    private Range<Integer> selectBestFpsRange(int requestedFps) {
        if (availableFpsRanges == null || availableFpsRanges.length == 0) {
            int safeFps = Math.max(1, requestedFps);
            return new Range<>(safeFps, safeFps);
        }

        Range<Integer> bestRange = availableFpsRanges[0];
        int bestDiff = Integer.MAX_VALUE;
        int bestSpan = Integer.MAX_VALUE;

        for (Range<Integer> range : availableFpsRanges) {
            int upper = range.getUpper();
            int lower = range.getLower();

            if (upper == requestedFps && lower == requestedFps) {
                return range;
            }

            int diff = Math.abs(upper - requestedFps);
            int span = upper - lower;

            // If requestedFps is covered by [lower, upper], favor it over an out-of-range fixed rate
            if (requestedFps >= lower && requestedFps <= upper) {
                diff = Math.min(diff, Math.abs(lower - requestedFps));
            }

            if (diff < bestDiff) {
                bestDiff = diff;
                bestSpan = span;
                bestRange = range;
            } else if (diff == bestDiff) {
                if (span < bestSpan) {
                    bestSpan = span;
                    bestRange = range;
                }
            }
        }
        return bestRange;
    }

    public void setMaxFocusDiopters(float maxDiopters) {
        if (maxDiopters > 0.0f) {
            this.maxFocusDiopters = maxDiopters;
        }
    }

    public void setTemplateType(int template) {
        this.templateType = template;
    }

    public void applyControl(int controlId, int value, CameraDevice device, Surface activeSurface, int activeFps, CameraCaptureSession session, Handler handler) {
        switch (controlId) {
            case V4L2_CID_FOCUS_AUTO:
                stateFocusMode = (value == 1) ? CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO : CaptureRequest.CONTROL_AF_MODE_OFF;
                break;
            case V4L2_CID_FOCUS_ABSOLUTE:
                // Map V4L2 0-100 scale against actual hardware diopters
                stateFocusDistance = (value / 100.0f) * maxFocusDiopters;
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
            CaptureRequest.Builder builder = cameraDevice.createCaptureRequest(templateType);
            builder.addTarget(activeSurface);

            Range<Integer> targetRange = selectBestFpsRange(activeFps);
            builder.set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, targetRange);

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
