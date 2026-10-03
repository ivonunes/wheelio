//! wheelio-streamer: ALVR's server core with a VideoToolbox encoder, fed by the
//! frames our OpenXR runtime publishes from the CrossOver bottle.
//!
//! Data flow: runtime -> shared memory -> stream::run -> Encoder -> ALVR.

mod audio;
mod encoder;
mod session;
mod shared_frames;
mod stream;

use alvr_common::{info, warn, HEAD_ID};
use shared_frames::HostState;
use alvr_server_core::{ServerCoreContext, ServerCoreEvent};
use std::{
    sync::{
        atomic::{AtomicBool, AtomicU64, Ordering},
        mpsc::RecvTimeoutError,
        Arc,
    },
    thread::{self, JoinHandle},
    time::Duration,
};
use stream::StreamControl;

/// One JSON object per line on stdout; the host app reads these for its
/// status display. Everything else goes to the ALVR log file.
fn status(event: &str, detail: serde_json::Value) {
    let mut object = serde_json::json!({ "event": event });
    if let (Some(target), Some(extra)) = (object.as_object_mut(), detail.as_object()) {
        for (k, v) in extra {
            target.insert(k.clone(), v.clone());
        }
    }
    println!("{object}");
}

/// Replaces ALVR's panic hook, installed by init_logging. ALVR's hook (and its
/// error reporting) opens a native message dialog from a background thread,
/// which macOS refuses with a panic of its own; that panic reaches the hook,
/// which opens another dialog, and the process spins forever writing gigabytes
/// of crash log while the stream starves. Here: a dialog attempt is logged and
/// dropped (the underlying error was already logged), anything else exits so
/// the host app starts a fresh streamer.
fn install_panic_hook() {
    std::panic::set_hook(Box::new(|info| {
        let location = info
            .location()
            .map(|l| format!("{}:{}", l.file(), l.line()))
            .unwrap_or_default();
        let message = info
            .payload()
            .downcast_ref::<&str>()
            .map(|s| s.to_string())
            .or_else(|| info.payload().downcast_ref::<String>().cloned())
            .unwrap_or_default();
        if location.contains("/rfd-") {
            alvr_common::warn!("suppressed a native error dialog (see the error above)");
            return;
        }
        alvr_common::error!("streamer panicked at {location}: {message}; exiting so the host app restarts it");
        std::process::exit(101);
    }));
}

/// Set by SIGTERM/SIGINT. The host app stops the streamer this way when the
/// game exits; a clean exit disconnects the headset and releases the audio tap.
static STOP_REQUESTED: AtomicBool = AtomicBool::new(false);

extern "C" fn on_stop_signal(_signal: libc::c_int) {
    STOP_REQUESTED.store(true, Ordering::Relaxed);
}

fn main() {
    unsafe {
        libc::signal(libc::SIGTERM, on_stop_signal as *const () as usize);
        libc::signal(libc::SIGINT, on_stop_signal as *const () as usize);
    }

    let root = dirs::data_dir()
        .expect("no Application Support directory")
        .join("Wheelio")
        .join("alvr");
    std::fs::create_dir_all(&root).expect("cannot create the ALVR config directory");
    let layout = alvr_filesystem::Layout::new(&root);

    let options = session::parse_options(std::env::args().skip(1));
    session::apply_overrides(&layout.session(), &options).expect("cannot write session.json");

    alvr_server_core::initialize_environment(layout.clone());
    alvr_server_core::init_logging(Some(layout.session_log()), Some(layout.crash_log()));
    install_panic_hook();

    let settings = alvr_server_core::settings();
    let codec = settings.video.preferred_codec;
    let fps = settings.video.preferred_fps;
    let bitrate_bps = match settings.video.bitrate.mode {
        alvr_session::BitrateMode::ConstantMbps(mbps) => mbps * 1_000_000,
        alvr_session::BitrateMode::Adaptive { .. } => 30_000_000,
    };
    info!("wheelio-streamer: ALVR {} in {}", alvr_common::ALVR_VERSION.to_string(), root.display());

    // Created before the core so the device exists when a headset negotiates.
    // Failure is loud but not fatal: video still streams without sound.
    let mute_locally = settings.audio.game_audio.as_option().is_some_and(|c| c.mute_when_streaming);
    let game_audio = match audio::GameAudioCapture::start(mute_locally) {
        Ok(capture) => Some(capture),
        Err(e) => {
            alvr_common::error!("game audio unavailable: {e}");
            None
        }
    };

    if let Some(seconds) = std::env::var("WHEELIO_AUDIO_TEST").ok().and_then(|s| s.parse::<u64>().ok()) {
        if let Err(e) = audio::self_test(seconds) {
            alvr_common::error!("audio self-test: {e}");
        }
        return;
    }

    let (context, events) = ServerCoreContext::new();
    let context = Arc::new(context);

    let control = Arc::new(StreamControl {
        running: AtomicBool::new(false),
        latest_tracking_ns: AtomicU64::new(0),
        idr_requested: AtomicBool::new(true),
        host_state: parking_lot::Mutex::new(HostState::default()),
        host_state_dirty: AtomicBool::new(false),
    });
    let mut pump: Option<JoinHandle<()>> = None;

    // WHEELIO_ENCODE_TEST=<seconds> runs the frame pump without a headset:
    // frames from the game go through the encoder and, with WHEELIO_RECORD,
    // to disk. Nothing is sent anywhere.
    if let Some(seconds) = std::env::var("WHEELIO_ENCODE_TEST").ok().and_then(|s| s.parse::<u64>().ok()) {
        info!("encode test for {seconds} s");
        control.running.store(true, Ordering::Relaxed);
        let handle = thread::spawn({
            let context = Arc::clone(&context);
            let control = Arc::clone(&control);
            move || stream::run(context, control, codec, fps, bitrate_bps)
        });
        thread::sleep(Duration::from_secs(seconds));
        control.running.store(false, Ordering::Relaxed);
        handle.join().ok();
        return;
    }

    context.start_connection();
    info!("waiting for a headset");
    status("waiting", serde_json::json!({ "audio": game_audio.is_some() }));

    loop {
        if STOP_REQUESTED.load(Ordering::Relaxed) {
            info!("stop requested");
            break;
        }
        let event = match events.recv_timeout(Duration::from_millis(100)) {
            Ok(event) => event,
            Err(RecvTimeoutError::Timeout) => continue,
            Err(RecvTimeoutError::Disconnected) => break,
        };
        match event {
            ServerCoreEvent::ClientConnected => {
                info!("headset connected, streaming");
                status("connected", serde_json::json!({ "codec": format!("{codec:?}"), "fps": fps }));
                control.running.store(true, Ordering::Relaxed);
                control.idr_requested.store(true, Ordering::Relaxed);
                let context = Arc::clone(&context);
                let control = Arc::clone(&control);
                pump = Some(thread::spawn(move || stream::run(context, control, codec, fps, bitrate_bps)));
            }
            ServerCoreEvent::ClientDisconnected => {
                // One headset session per process. The audio library ALVR
                // records through never frees a stream on a non-default device
                // (a reference cycle in its disconnect listener), so the audio
                // sender and with it the UDP stream socket leak: the next
                // connection could not bind the port, and the Mac stayed
                // muted. Exiting frees everything; the host app starts a fresh
                // streamer while the game is still running.
                info!("headset disconnected; exiting so the next session starts clean");
                status("restarting", serde_json::json!({}));
                control.running.store(false, Ordering::Relaxed);
                if let Some(handle) = pump.take() {
                    handle.join().ok();
                }
                break;
            }
            ServerCoreEvent::Tracking { sample_timestamp } => {
                control
                    .latest_tracking_ns
                    .store(sample_timestamp.as_nanos() as u64, Ordering::Relaxed);
                // ALVR already predicts these poses to the headset's display
                // time, so the runtime renders with them as they are.
                if let Some(motion) = context.get_device_motion(*HEAD_ID, sample_timestamp) {
                    // A pose every few seconds in the log shows whether the head is moving at all.
                    static LAST_LOGGED: AtomicU64 = AtomicU64::new(0);
                    let now_s = sample_timestamp.as_secs();
                    if now_s.abs_diff(LAST_LOGGED.load(Ordering::Relaxed)) >= 5 {
                        LAST_LOGGED.store(now_s, Ordering::Relaxed);
                        info!("head pose: {:?} at {:?}", motion.pose, sample_timestamp);
                    }
                    let mut state = control.host_state.lock();
                    state.tracking_timestamp_ns = sample_timestamp.as_nanos() as u64;
                    state.head_orientation = motion.pose.orientation.to_array();
                    state.head_position = motion.pose.position.to_array();
                    state.linear_velocity = motion.linear_velocity.to_array();
                    state.angular_velocity = motion.angular_velocity.to_array();
                    state.has_pose = true;
                    control.host_state_dirty.store(true, Ordering::Release);
                }
            }
            ServerCoreEvent::ViewsConfig(config) => {
                let ipd = config.local_view_transforms[1].position.x - config.local_view_transforms[0].position.x;
                info!("headset views: ipd {:.1} mm, fov left {:?}, right {:?}", ipd * 1000.0, config.fov[0], config.fov[1]);
                shared_frames::save_headset_optics(&config.fov, ipd);
                let mut state = control.host_state.lock();
                for eye in 0..2 {
                    state.fov[eye] = [config.fov[eye].left, config.fov[eye].right, config.fov[eye].up, config.fov[eye].down];
                }
                state.ipd_m = ipd;
                state.has_views = true;
                control.host_state_dirty.store(true, Ordering::Release);
            }
            ServerCoreEvent::PlayspaceSync(_) => {
                // Sent when the headset recentres; the runtime moves its LOCAL origin.
                let mut state = control.host_state.lock();
                state.recenter_generation += 1;
                control.host_state_dirty.store(true, Ordering::Release);
                info!("headset recentred (generation {})", state.recenter_generation);
            }
            ServerCoreEvent::RequestIDR => {
                control.idr_requested.store(true, Ordering::Relaxed);
            }
            ServerCoreEvent::Battery(_) | ServerCoreEvent::Buttons(_) => {}
            ServerCoreEvent::SetOpenvrProperty { .. }
            | ServerCoreEvent::CaptureFrame
            | ServerCoreEvent::GameRenderLatencyFeedback(_) => {}
            ServerCoreEvent::ShutdownPending | ServerCoreEvent::RestartPending => {
                warn!("ALVR requested shutdown");
                break;
            }
        }
    }

    control.running.store(false, Ordering::Relaxed);
    if let Some(handle) = pump.take() {
        handle.join().ok();
    }
    // Dropping the core disconnects the headset cleanly; dropping the capture
    // destroys the tap and unmutes the Mac.
    drop(game_audio);
    drop(context);
}
