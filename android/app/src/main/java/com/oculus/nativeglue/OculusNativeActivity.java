package com.oculus.nativeglue;

import android.app.NativeActivity;
import android.content.Intent;
import android.os.Bundle;
import java.util.Set;

/**
 * Activity class the Meta runtime recognises: the legacy Oculus Native SDK
 * glue name. VrRuntimeClient (in-process) reflects a getLaunchId method on
 * the activity's own class to answer vrshell's launch check; a stock
 * android.app.NativeActivity has none, the lookup throws, and the runtime
 * policy then destroys the OpenXR RuntimeInterface ~10ms after
 * xrBeginSession (the frame pump aborts on the freed client mutex).
 *
 * The launch id arrives as a long extra on the activity intent when
 * vrshell launches the tile (com.oculus.launch_check.extra.ACTIVITY_UUID);
 * direct `am start` launches have none and report 0.
 */
public class OculusNativeActivity extends NativeActivity {
    private static volatile long sLaunchId = 0;

    private static long readLaunchId(Intent intent) {
        if (intent == null) return 0;
        long id = intent.getLongExtra("com.oculus.launch_check.extra.ACTIVITY_UUID", 0);
        if (id != 0) return id;
        Bundle b = intent.getExtras();
        if (b == null) return 0;
        Set<String> keys = b.keySet();
        for (String k : keys) {
            if (k.contains("UUID") || k.contains("LAUNCH_ID")) {
                Object v = b.get(k);
                if (v instanceof Long && (Long) v != 0) return (Long) v;
            }
        }
        return 0;
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        sLaunchId = readLaunchId(getIntent());
    }

    /** Reflection targets, both static and instance lookup shapes. */
    public static long getLaunchId() { return sLaunchId; }

    public static long getLaunchId(Object... args) {
        if (sLaunchId == 0 && args != null && args.length > 0
                && args[0] instanceof android.app.Activity)
            sLaunchId = readLaunchId(((android.app.Activity) args[0]).getIntent());
        return sLaunchId;
    }

    public long getLaunchIdInstance() { return sLaunchId; }
}
