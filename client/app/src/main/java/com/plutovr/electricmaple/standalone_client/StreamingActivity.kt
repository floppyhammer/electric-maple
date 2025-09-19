// Copyright 2023, Pluto VR, Inc.
//
// SPDX-License-Identifier: BSL-1.0

package com.plutovr.electricmaple.standalone_client

import org.freedesktop.gstreamer.GStreamer

import android.app.NativeActivity
import android.os.Bundle
import android.os.PersistableBundle
import android.util.Log
import android.view.WindowManager

class StreamingActivity : NativeActivity() {
    override fun onCreate(savedInstanceState: Bundle?, persistentState: PersistableBundle?) {
        super.onCreate(savedInstanceState, persistentState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        System.loadLibrary("electricmaple_client")
        Log.i("ElectricMaple", "StreamingActivity: loaded electricmaple_client")
        System.loadLibrary("electricmaple_standalone_client")
        Log.i("ElectricMaple", "StreamingActivity: loaded")
    }

    companion object {
        init {
            Log.i("ElectricMaple", "StreamingActivity: In StreamingActivity static init")
        }
    }
}
