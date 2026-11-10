/*
 * SPDX-License-Identifier: Apache-2.0
 */
package org.gtelwifi.parts;

import android.content.Context;
import android.content.SharedPreferences;
import android.util.Log;

import java.io.FileWriter;
import java.io.IOException;

/**
 * The kernel caps the CPU at 1300 MHz (vendor maximum) through the
 * cpu_max_khz parameter of mach-sprd/sc7730.c; the device tree offers steps
 * up to 1612 MHz, which ran stable on the SM-T560 (max. 74 C). The thermal
 * driver still throttles from 85 C on.
 */
final class Cpu {
    private static final String TAG = "GtelwifiParts";
    private static final String PATH = "/sys/module/sc7730/parameters/cpu_max_khz";
    static final String KEY_OVERCLOCK = "cpu_overclock";
    private static final String STOCK_KHZ = "1300000";
    private static final String OC_KHZ = "1612000";

    private Cpu() {}

    static SharedPreferences prefs(Context context) {
        return context.createDeviceProtectedStorageContext()
                .getSharedPreferences(context.getPackageName() + "_preferences",
                        Context.MODE_PRIVATE);
    }

    static void apply(boolean overclock) {
        try (FileWriter w = new FileWriter(PATH)) {
            w.write(overclock ? OC_KHZ : STOCK_KHZ);
        } catch (IOException e) {
            Log.e(TAG, "cannot write " + PATH, e);
        }
    }
}
