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
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        System.loadLibrary("electric_maple")
        Log.i("ElectricMaple", "StreamingActivity: loaded electric_maple")
        System.loadLibrary("electric_maple_client")
        Log.i("ElectricMaple", "StreamingActivity: loaded electric_maple_client")

        super.onCreate(savedInstanceState, persistentState)
    }

    companion object {
        init {
            Log.i("ElectricMaple", "StreamingActivity: In StreamingActivity static init")
        }
    }
}
