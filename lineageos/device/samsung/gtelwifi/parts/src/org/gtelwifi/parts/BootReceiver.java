/*
 * SPDX-License-Identifier: Apache-2.0
 */
package org.gtelwifi.parts;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;

/** Restores the CPU limit the user chose; the kernel boots with 1300 MHz. */
public class BootReceiver extends BroadcastReceiver {
    @Override
    public void onReceive(Context context, Intent intent) {
        if (Cpu.prefs(context).getBoolean(Cpu.KEY_OVERCLOCK, false)) {
            Cpu.apply(true);
        }
    }
}
