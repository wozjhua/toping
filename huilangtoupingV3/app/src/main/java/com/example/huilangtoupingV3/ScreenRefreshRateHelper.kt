package com.example.huilangtoupingV3

import android.content.Context
import android.hardware.display.DisplayManager
import android.os.Build
import android.view.Display

object ScreenRefreshRateHelper {
    fun currentRefreshRate(context: Context): Float {
        return runCatching {
            val appContext = context.applicationContext
            val displayManager = appContext.getSystemService(Context.DISPLAY_SERVICE) as DisplayManager
            val display = displayManager.getDisplay(Display.DEFAULT_DISPLAY)
                ?: return@runCatching 0f

            @Suppress("DEPRECATION")
            val currentRefreshRate = display.refreshRate
            if (currentRefreshRate > 0f) {
                currentRefreshRate
            } else if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
                display.mode?.refreshRate ?: 0f
            } else {
                0f
            }
        }.getOrDefault(0f)
    }
}
