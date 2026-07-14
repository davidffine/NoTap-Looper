package com.notap.looper.ui.state

import android.content.Context
import android.content.SharedPreferences

/**
 * Tiny persistence wrapper — the app's only cross-launch state.
 * Kept deliberately dumb: two flags. Anything richer (DataStore, cloud sync)
 * replaces this class behind the same three members.
 */
class Prefs(context: Context) {
    private val sp: SharedPreferences =
        context.applicationContext.getSharedPreferences("notap_prefs", Context.MODE_PRIVATE)

    var hasOnboarded: Boolean
        get() = sp.getBoolean("has_onboarded", false)
        set(v) = sp.edit().putBoolean("has_onboarded", v).apply()

    var isPro: Boolean
        get() = sp.getBoolean("is_pro", false)
        set(v) = sp.edit().putBoolean("is_pro", v).apply()
}
