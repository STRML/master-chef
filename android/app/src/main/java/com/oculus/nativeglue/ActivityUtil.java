package com.oculus.nativeglue;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import java.util.Set;

/**
 * Minimal stand-in for the Oculus Mobile SDK's nativeglue class.
 *
 * The Meta runtime's in-process VrRuntimeClient resolves the launch id by
 * reflecting ActivityUtil.getLaunchId(...) through the app's class loader.
 * A pure-native APK (android:hasCode="false") has no dex at all, the
 * reflection throws, and the runtime's launch-check policy tears down the
 * OpenXR RuntimeInterface ~10ms after the session begins - the frame pump
 * then aborts on the destroyed client mutex (FORTIFY). This class answers
 * the reflection with the id vrshell passed in the activity intent
 * (icon-tap / Library launches), or 0 for direct am start launches.
 */
public final class ActivityUtil {
    private static volatile long sLaunchId = 0;

    private ActivityUtil() { }

    /** Called from android_main once the activity intent is known. */
    public static void initialize(Activity activity) {
        long id = 0;
        if (activity != null && activity.getIntent() != null) {
            Intent in = activity.getIntent();
            id = in.getLongExtra("com.oculus.launch_check.extra.ACTIVITY_UUID", 0);
            if (id == 0) {
                Bundle b = in.getExtras();
                if (b != null) {
                    Set<String> keys = b.keySet();
                    for (String k : keys) {
                        if (k.contains("UUID") || k.contains("LAUNCH_ID")) {
                            Object v = b.get(k);
                            if (v instanceof Long) { id = (Long) v; break; }
                        }
                    }
                }
            }
        }
        sLaunchId = id;
    }

    public static void setLaunchId(long id) { sLaunchId = id; }

    public static long getLaunchId() { return sLaunchId; }
    public static long getLaunchId(Activity activity) {
        if (activity != null && sLaunchId == 0) initialize(activity);
        return sLaunchId;
    }

    /** JVMHandler.callStaticMethodWithVariableArguments looks the method up
     *  as getLaunchId([Ljava/lang/Object;)J; accept that exact signature. */
    public static long getLaunchId(Object... args) {
        if (sLaunchId == 0 && args != null && args.length > 0
                && args[0] instanceof Activity)
            initialize((Activity) args[0]);
        return sLaunchId;
    }
}
