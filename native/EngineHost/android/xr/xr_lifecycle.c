/*
 * xr_lifecycle.c - Android activity lifecycle state machine.
 *
 * Maps XrSessionState events + Android activity commands (onResume /
 * onPause / back) onto the coarse app state the host uses to decide
 * when to render, when to suspend, and when to tear down. Pure
 * transitions; no XR runtime calls (host-testable).
 */
#include "xr.h"

void xr_lc_init(xr_lc *lc) {
    lc->state = XR_APP_NEW;
    lc->actions = XR_LC_ACT_NONE;
    lc->focused = false;
}


/* Map a raw XrSessionState to the FSM event it triggers (0 if none).
 * The OpenXR session state diagram (Android):
 * IDLE -> READY -> SYNCHRONIZED -> VISIBLE -> RUNNING -> STOPPING
 * -> CLOSING -> EXITING; the runtime drives the session, the app
 * reacts to focus loss and back. */
xr_lc_event xr_lc_event_from_session_state(XrSessionState s) {
    /* CLOSING and DESTROYED are terminal teardown states; both map to the
     * same closing event. The Meta runtime sends them during teardown. */
    if (s == XR_SESSION_STATE_CLOSING || s == XR_SESSION_STATE_DESTROYED)
        return XR_LC_SESSION_CLOSING;
    switch (s) {
    case XR_SESSION_STATE_READY:          return XR_LC_SESSION_READY;
    case XR_SESSION_STATE_SYNCHRONIZED:
    case XR_SESSION_STATE_VISIBLE:
    case XR_SESSION_STATE_FOCUSED:       return XR_LC_SESSION_RUNNING;
    case XR_SESSION_STATE_STOPPING:      return XR_LC_SESSION_STOPPING;
    case XR_SESSION_STATE_LOSS_PENDING:  return XR_LC_INSTANCE_LOSS;
    case XR_SESSION_STATE_EXITING:       return XR_LC_SESSION_EXITING;
    default:                             return 0;
    }
}

/* Feed one event; returns 0 if accepted, -1 if invalid for the
 * current state. */
int xr_lc_send(xr_lc *lc, xr_lc_event ev) {
    if (ev == 0) return 0;
    if (ev == XR_LC_INSTANCE_LOSS) {
        lc->state = XR_APP_EXITING;
        lc->actions = XR_LC_ACT_END | XR_LC_ACT_QUIT;
        return 0;
    }
    switch (lc->state) {
    case XR_APP_NEW:
        if (ev == XR_LC_SESSION_READY) {
            lc->state = XR_APP_RUNNING;
            lc->actions = XR_LC_ACT_BEGIN | XR_LC_ACT_ENQUEUE;
            lc->focused = true;
            return 0;
        }
        break;
    case XR_APP_READY:
        if (ev == XR_LC_SESSION_RUNNING) {
            lc->state = XR_APP_RUNNING;
            lc->actions = XR_LC_ACT_ENQUEUE;
            return 0;
        }
        if (ev == XR_LC_QUIT) {
            lc->state = XR_APP_EXITING;
            lc->actions = XR_LC_ACT_QUIT;
            return 0;
        }
        break;
    case XR_APP_RUNNING:
        if (ev == XR_LC_FOCUS_LOST || ev == XR_LC_SESSION_STOPPING) {
            lc->state = XR_APP_PAUSED;
            lc->actions = XR_LC_ACT_SUSPEND;
            lc->focused = false;
            return 0;
        }
        if (ev == XR_LC_QUIT || ev == XR_LC_SESSION_EXITING) {
            lc->state = XR_APP_EXITING;
            lc->actions = XR_LC_ACT_SUSPEND | XR_LC_ACT_QUIT;
            return 0;
        }
        if (ev == XR_LC_SESSION_RUNNING || ev == XR_LC_SESSION_READY) {
            lc->actions = XR_LC_ACT_ENQUEUE;
            return 0;
        }
        break;
    case XR_APP_PAUSED:
        if (ev == XR_LC_FOCUS_GAINED || ev == XR_LC_SESSION_RUNNING) {
            lc->state = XR_APP_RUNNING;
            lc->actions = XR_LC_ACT_ENQUEUE;
            lc->focused = true;
            return 0;
        }
        if (ev == XR_LC_SESSION_READY) {
            lc->state = XR_APP_RUNNING;
            lc->actions = XR_LC_ACT_BEGIN | XR_LC_ACT_ENQUEUE;
            lc->focused = true;
            return 0;
        }
        if (ev == XR_LC_SESSION_STOPPING || ev == XR_LC_SESSION_EXITING || ev == XR_LC_QUIT) {
            lc->state = XR_APP_EXITING;
            lc->actions = XR_LC_ACT_END | XR_LC_ACT_QUIT;
            return 0;
        }
        break;
    case XR_APP_EXITING:
        if (ev == XR_LC_SESSION_CLOSING || ev == XR_LC_SESSION_EXITING || ev == XR_LC_QUIT) {
            lc->state = XR_APP_EXITED;
            lc->actions = XR_LC_ACT_QUIT;
            return 0;
        }
        break;
    case XR_APP_EXITED:
        break;
    }
    return -1;
}
