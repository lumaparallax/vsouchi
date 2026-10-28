/*
 * Copyright (C) 2026 Luma Parallax
 * SPDX-License-Identifier: GPL-3.0-only
 */

package io.github.lumaparallax.vsouchi.v4l2;

import android.content.Context;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraManager;
import android.hardware.camera2.params.StreamConfigurationMap;
import android.os.Build;
import android.util.Size;
import io.github.lumaparallax.vsouchi.Logger;

public class CameraHardwareManager {
    private static final String TAG = "vSouchi:V4L2";
    private final Context context;
    private final NativeBridge bridge;

    public CameraHardwareManager(Context context, NativeBridge bridge) {
        this.context = context;
        this.bridge = bridge;
    }

    public void probeAndPushCapabilities() {
        try {
            CameraManager manager = (CameraManager) context.getSystemService(Context.CAMERA_SERVICE);
            String cameraId = manager.getCameraIdList()[0];
            CameraCharacteristics chars = manager.getCameraCharacteristics(cameraId);

            Integer facing = chars.get(CameraCharacteristics.LENS_FACING);
            String lensStr = "Camera";
            if (facing != null) {
                if (facing == CameraCharacteristics.LENS_FACING_BACK) lensStr = "Back Camera";
                else if (facing == CameraCharacteristics.LENS_FACING_FRONT) lensStr = "Front Camera";
                else if (facing == CameraCharacteristics.LENS_FACING_EXTERNAL) lensStr = "External Camera";
            }
            
            String deviceName = Build.MANUFACTURER + " " + Build.MODEL + " " + lensStr;
            bridge.nativeSetDeviceInfo(deviceName);

            StreamConfigurationMap map = chars.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);

            if (map != null) {
                int[] formats = map.getOutputFormats();
                if (formats != null && formats.length > 0) {
                    bridge.nativeSetFormats(formats);

                    int totalResCount = 0;
                    for (int fmt : formats) {
                        Size[] sizes = map.getOutputSizes(fmt);
                        if (sizes != null) totalResCount += sizes.length;
                    }

                    int[] resFmts = new int[totalResCount];
                    int[] resWs = new int[totalResCount];
                    int[] resHs = new int[totalResCount];
                    int[] resFps = new int[totalResCount];

                    int idx = 0;
                    for (int fmt : formats) {
                        Size[] sizes = map.getOutputSizes(fmt);
                        if (sizes != null) {
                            for (Size s : sizes) {
                                resFmts[idx] = fmt;
                                resWs[idx] = s.getWidth();
                                resHs[idx] = s.getHeight();
                                
                                long minFrameDuration = map.getOutputMinFrameDuration(fmt, s);
                                if (minFrameDuration > 0) {
                                    resFps[idx] = (int) Math.round(1_000_000_000.0 / minFrameDuration);
                                } else {
                                    resFps[idx] = 30;
                                }
                                idx++;
                            }
                        }
                    }
                    
                    bridge.nativeSetResolutions(resFmts, resWs, resHs, resFps);
                    Logger.i(TAG, "Pushed " + totalResCount + " format-aware resolutions to Native Cache.");

                    Float minFocus = chars.get(CameraCharacteristics.LENS_INFO_MINIMUM_FOCUS_DISTANCE);
                    boolean hasFocus = (minFocus != null && minFocus > 0.0f);
                    float focusMaxVal = hasFocus ? minFocus : 0.0f;

                    android.util.Range<Long> expRange = chars.get(CameraCharacteristics.SENSOR_INFO_EXPOSURE_TIME_RANGE);
                    boolean hasExposure = (expRange != null);
                    long minExp = hasExposure ? expRange.getLower() : 0;
                    long maxExp = hasExposure ? expRange.getUpper() : 0;

                    Integer sensorOrientation = chars.get(CameraCharacteristics.SENSOR_ORIENTATION);
                    int orientation = (sensorOrientation != null) ? sensorOrientation : 0;
                    int hflip = (facing != null && facing == CameraCharacteristics.LENS_FACING_FRONT) ? 1 : 0;

                    bridge.nativeSetCameraCaps(hasFocus, focusMaxVal, hasExposure, minExp, maxExp, orientation, hflip);
                    Logger.i(TAG, "Hardware Caps pushed to C-Bridge on boot.");
                }
            }

        } catch (Exception e) {
            Logger.e(TAG, "Failed to get camera resolutions", e);
        }
    }
}
