#if TARGET_OS_VISION
/* The coordinator owns observer tokens for the process lifetime. All session
 * mutations and queue recovery run on its serial queue, never the render callback.
 * AudioQueue clients must explicitly reactivate after interruption and replace
 * orphaned queues after AVAudioSessionMediaServicesWereResetNotification. */
static dispatch_queue_t audio_control_queue;
static dispatch_source_t audio_watchdog_timer;
static NSArray *audio_observers;
static BOOL audio_interrupted, audio_resume_allowed=YES, audio_needs_rebuild;
static BOOL audio_foreground=YES, audio_resume_pending;
static uint64_t audio_retry_after_ns;

static BOOL audio_session_can_resume(void) {
    int state=enginevision_runtime_state();
    return (state==ENGINEVISION_STARTING || state==ENGINEVISION_RUNNING) &&
        audio_foreground && !audio_interrupted && audio_resume_allowed;
}

/* Serialized, privacy-minimized: fixed system port types, never device names/UIDs. */
static void audio_session_route(const char *reason) {
    static unsigned reports;if(reports++>=64)return;
    AVAudioSession *session=[AVAudioSession sharedInstance];
    NSMutableArray *types=[NSMutableArray array];
    for(AVAudioSessionPortDescription *port in session.currentRoute.outputs)[types addObject:port.portType];
    fprintf(stderr,"[audio-route] event=%s types=%s volume=%.3f rate=%.0f channels=%ld interrupted=%d resumeAllowed=%d rebuild=%d\n",
        reason,[types componentsJoinedByString:@","].UTF8String,session.outputVolume,session.sampleRate,
        (long)session.outputNumberOfChannels,audio_interrupted,audio_resume_allowed,audio_needs_rebuild);
}
static void audio_session_resume(const char *reason) {
    if (!audio_session_can_resume()) return;
    NSError *error=nil;
    AVAudioSession *session=[AVAudioSession sharedInstance];
    BOOL ready=[session setCategory:AVAudioSessionCategoryPlayback
                              mode:AVAudioSessionModeDefault options:0 error:&error];
    if (ready) ready=[session setActive:YES error:&error];
    if (ready && audio_needs_rebuild) {
        ready=host_dsound_rebuild_output()!=0;
        if (ready) audio_needs_rebuild=NO;
    }
    if (ready) ready=host_dsound_resume_output()!=0;
    audio_resume_pending=!ready;
    if (!ready) {
        /* Failed activation can leave the output deliberately suspended. The
         * queue-stall watchdog cannot repair that state, so retry the entire
         * session transaction on the control timer, with bounded frequency. */
        host_dsound_pause_output();
        audio_retry_after_ns=clock_gettime_nsec_np(CLOCK_UPTIME_RAW)+2000000000ull;
    }
    audio_session_route(reason);
    fprintf(stderr,"[audio-session] %s playback=%s %s\n",reason,ready?"active":"failed",
            error?error.localizedDescription.UTF8String:"");
}

static void audio_session_tick(void) {
    if (!audio_session_can_resume()) return;
    if (audio_resume_pending) {
        if (clock_gettime_nsec_np(CLOCK_UPTIME_RAW)>=audio_retry_after_ns)
            audio_session_resume("activation-retry");
    } else host_dsound_watchdog();
}

static void audio_session_prepare(void) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        audio_control_queue=dispatch_queue_create("HaloVision.audio-session",DISPATCH_QUEUE_SERIAL);
        /* The stall watchdog ran once per engine frame, so it slept through
         * exactly the stalls it exists for: level loads and the engine's own
         * stops (Build73's queue stopped at 290.4 s and nothing recovered it).
         * Run it here, serialized with every other queue transition. */
        host_dsound_watchdog_set_external();
        audio_watchdog_timer=dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER,0,0,audio_control_queue);
        dispatch_source_set_timer(audio_watchdog_timer,dispatch_time(DISPATCH_TIME_NOW,250*NSEC_PER_MSEC),
                                  250*NSEC_PER_MSEC,50*NSEC_PER_MSEC);
        dispatch_source_set_event_handler(audio_watchdog_timer,^{ audio_session_tick(); });
        dispatch_resume(audio_watchdog_timer);
        NSNotificationCenter *center=[NSNotificationCenter defaultCenter];
        id interruption=[center addObserverForName:AVAudioSessionInterruptionNotification
            object:[AVAudioSession sharedInstance] queue:nil usingBlock:^(NSNotification *note) {
                NSNumber *type=note.userInfo[AVAudioSessionInterruptionTypeKey];
                NSNumber *options=note.userInfo[AVAudioSessionInterruptionOptionKey];
                if (!type) return;
                dispatch_async(audio_control_queue, ^{
                    if (type.unsignedIntegerValue==AVAudioSessionInterruptionTypeBegan) {
                        audio_interrupted=YES;
                        audio_session_route("interruption-began");
                        host_dsound_pause_output();
                        fprintf(stderr,"[audio-session] interruption began\n");
                    } else if (type.unsignedIntegerValue==AVAudioSessionInterruptionTypeEnded) {
                        audio_interrupted=NO;
                        audio_resume_allowed=(options.unsignedIntegerValue & AVAudioSessionInterruptionOptionShouldResume)!=0;
                        fprintf(stderr,"[audio-session] interruption ended shouldResume=%s\n",audio_resume_allowed?"yes":"no");
                        audio_session_resume("interruption-ended");
                    }
                });
            }];
        id reset=[center addObserverForName:AVAudioSessionMediaServicesWereResetNotification
            object:[AVAudioSession sharedInstance] queue:nil usingBlock:^(NSNotification *note) {
                (void)note;
                dispatch_async(audio_control_queue, ^{
                    audio_needs_rebuild=YES;
                    audio_session_resume("media-services-reset");
                });
            }];
        id foreground=[center addObserverForName:UIApplicationDidBecomeActiveNotification
            object:nil queue:nil usingBlock:^(NSNotification *note) {
                (void)note;
                dispatch_async(audio_control_queue, ^{
                    /* A new foreground transition may have no matching ended
                     * interruption. Coming back to the game is the player's own
                     * action, so it also clears an interruption's
                     * shouldResume=NO; keeping that left the mix silent for
                     * the rest of the session. */
                    audio_interrupted=NO;
                    audio_resume_allowed=YES;
                    audio_foreground=YES;
                    audio_session_resume("foreground-active");
                });
            }];
        id background=[center addObserverForName:UIApplicationDidEnterBackgroundNotification
            object:nil queue:nil usingBlock:^(NSNotification *note) {
                (void)note;
                dispatch_async(audio_control_queue, ^{
                    /* The app has no audio background mode, so the system
                     * suspends it shortly. A queue left running stalls there,
                     * and every rebuild in the background fails (Build72:
                     * three, OSStatus 'what'). Pause cleanly; the foreground
                     * transition restarts it. */
                    audio_foreground=NO;
                    host_dsound_pause_output();
                    audio_session_route("background");
                });
            }];
        id route=[center addObserverForName:AVAudioSessionRouteChangeNotification object:[AVAudioSession sharedInstance] queue:nil usingBlock:^(NSNotification *note) {
            NSNumber *reason=note.userInfo[AVAudioSessionRouteChangeReasonKey];
            dispatch_async(audio_control_queue, ^{char event[48];snprintf(event,sizeof event,"route-change-%lu",(unsigned long)reason.unsignedIntegerValue);audio_session_route(event);});
        }];
        audio_observers=@[interruption,reset,foreground,background,route];
    });
    dispatch_sync(audio_control_queue, ^{ audio_session_resume("startup"); });
}
#endif
