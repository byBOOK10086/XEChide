package dev.xechide.manager

import android.app.Application

/**
 * The app has no process-wide mutable state. This class exists so manifest
 * startup and future diagnostics have a stable hook; all real state is either
 * in Compose or in [dev.xechide.manager.data.SecureStore].
 */
class XeChideApp : Application()
