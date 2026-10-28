/*
 * Copyright (C) 2026 Luma Parallax
 * SPDX-License-Identifier: GPL-3.0-only
 */

package io.github.lumaparallax.vsouchi.v4l2;

import android.content.Context;
import android.graphics.ImageFormat;
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

    private String resolveCameraRole(CameraCharacteristics chars, Integer facing) {
        if (facing != null && facing == CameraCharacteristics.LENS_FACING_FRONT) {
            return "Front Selfie";
        }
        if (facing != null && facing == CameraCharacteristics.LENS_FACING_EXTERNAL) {
            return "External";
        }

        float[] focalLengths = chars.get(CameraCharacteristics.LENS_INFO_AVAILABLE_FOCAL_LENGTHS);
        android.util.SizeF sensorSize = chars.get(CameraCharacteristics.SENSOR_INFO_PHYSICAL_SIZE);

        if (focalLengths != null && focalLengths.length > 0 && sensorSize != null && sensorSize.getWidth() > 0) {
            double hFov = 2.0 * Math.toDegrees(Math.atan(sensorSize.getWidth() / (2.0 * focalLengths[0])));
            if (hFov >= 85.0) return "Rear Ultra-Wide";
            if (hFov <= 45.0) return "Rear Telephoto";
        }
        return "Rear Main";
    }

    public void probeAndPushCapabilities() {
        try {
            CameraManager manager = (CameraManager) context.getSystemService(Context.CAMERA_SERVICE);
            String cameraId = manager.getCameraIdList()[0];
            CameraCharacteristics chars = manager.getCameraCharacteristics(cameraId);

            Integer facing = chars.get(CameraCharacteristics.LENS_FACING);
            String role = resolveCameraRole(chars, facing);

            int megapixels = 0;
            Size pixelArray = chars.get(CameraCharacteristics.SENSOR_INFO_PIXEL_ARRAY_SIZE);
            if (pixelArray != null) {
                megapixels = (int) Math.round((pixelArray.getWidth() * (double) pixelArray.getHeight()) / 1_000_000.0);
            }

            String rawMfr = Build.MANUFACTURER;
            String brand = rawMfr.substring(0, 1).toUpperCase() + rawMfr.substring(1);

            String deviceName;
            if (megapixels > 0) {
                deviceName = brand + " " + role + " (" + megapixels + "MP)";
            } else {
                deviceName = brand + " " + role;
            }

            if (deviceName.length() > 31) {
                deviceName = deviceName.substring(0, 31);
            }

            bridge.nativeSetDeviceInfo(deviceName, cameraId);

            StreamConfigurationMap map = chars.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);

            if (map != null) {
                int[] rawFormats = map.getOutputFormats();
                boolean hasYuv = false;
                boolean hasJpeg = false;
                boolean hasRaw = false;
                if (rawFormats != null) {
                    /* Filter to formats supported by the native V4L2 bridge and prioritize YUV_420_888
                     * so unsupported formats (e.g. PRIVATE, DEPTH16, HEIC) do not exhaust the 256-entry table.*/
                    for (int fmt : rawFormats) {
                        if (fmt == ImageFormat.YUV_420_888) hasYuv = true;
                        else if (fmt == ImageFormat.JPEG) hasJpeg = true;
                        else if (fmt == ImageFormat.RAW_SENSOR) hasRaw = true;
                    }
                }

                int supportedCount = (hasYuv ? 1 : 0) + (hasJpeg ? 1 : 0) + (hasRaw ? 1 : 0);
                if (supportedCount > 0) {
                    int[] formats = new int[supportedCount];
                    int fmtIdx = 0;
                    if (hasYuv) formats[fmtIdx++] = ImageFormat.YUV_420_888;
                    if (hasJpeg) formats[fmtIdx++] = ImageFormat.JPEG;
                    if (hasRaw) formats[fmtIdx++] = ImageFormat.RAW_SENSOR;

                    Integer cfa = chars.get(CameraCharacteristics.SENSOR_INFO_COLOR_FILTER_ARRANGEMENT);
                    bridge.nativeSetCfaPattern(cfa != null ? cfa : 0);
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
