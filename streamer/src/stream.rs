//! The frame pump: waits for the runtime to publish a frame, copies it into
//! the encoder, and hands the encoded NAL units to ALVR.

use crate::{
    encoder::Encoder,
    session::{VIEW_HEIGHT, VIEW_WIDTH},
    shared_frames::{self, SharedFrames},
};
use alvr_common::{error, info, warn};
use alvr_server_core::ServerCoreContext;
use alvr_session::CodecType;
use std::{
    fs::File,
    io::Write,
    sync::{
        atomic::{AtomicBool, AtomicU64, Ordering},
        mpsc::RecvTimeoutError,
        Arc,
    },
    thread,
    time::{Duration, Instant, SystemTime, UNIX_EPOCH},
};

/// State the ALVR event loop shares with the pump.
pub struct StreamControl {
    pub running: AtomicBool,
    /// Latest tracking timestamp from the client, in nanoseconds. Used to stamp
    /// frames only while the runtime has not yet reported which sample it
    /// rendered with.
    pub latest_tracking_ns: AtomicU64,
    pub idr_requested: AtomicBool,
    /// Head pose and optics for the runtime, written into the shared buffer
    /// by the pump whenever the event loop updates them.
    pub host_state: parking_lot::Mutex<shared_frames::HostState>,
    pub host_state_dirty: AtomicBool,
}

fn wall_now_unix_ns() -> u64 {
    SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_nanos() as u64).unwrap_or(0)
}

pub fn run(context: Arc<ServerCoreContext>, control: Arc<StreamControl>, codec: CodecType, fps: f32, bitrate_bps: u64) {
    let (view_width, view_height) = (VIEW_WIDTH, VIEW_HEIGHT);
    let width = view_width * 2;
    let height = view_height;
    let (mut encoder, encoded) = match Encoder::new(codec, width, height, fps, bitrate_bps) {
        Ok(pair) => pair,
        Err(e) => {
            error!("encoder: {e}");
            return;
        }
    };
    info!("encoder ready: {codec:?} {width}x{height} @ {fps} fps, {} Mbps", bitrate_bps / 1_000_000);

    // Deliver encoded frames to ALVR from their own thread so a slow network
    // path never blocks the encode submission.
    let sender_thread = thread::spawn({
        let context = Arc::clone(&context);
        let control = Arc::clone(&control);
        move || {
            let mut sent_config = false;
            // WHEELIO_RECORD=<path> writes the raw Annex B stream to disk so the
            // encoder output can be checked with ffmpeg without a headset.
            let mut recording = std::env::var_os("WHEELIO_RECORD").and_then(|path| match File::create(&path) {
                Ok(file) => {
                    info!("recording stream to {}", path.to_string_lossy());
                    Some(file)
                }
                Err(e) => {
                    warn!("cannot record to {}: {e}", path.to_string_lossy());
                    None
                }
            });
            let mut stats_started = Instant::now();
            let mut stats_frames = 0u32;
            let mut stats_bytes = 0usize;
            let mut stats_encode = Duration::ZERO;
            while control.running.load(Ordering::Relaxed) {
                let frame = match encoded.recv_timeout(Duration::from_millis(100)) {
                    Ok(frame) => frame,
                    Err(RecvTimeoutError::Timeout) => continue,
                    Err(RecvTimeoutError::Disconnected) => break,
                };
                if let Some(config) = frame.config {
                    if !sent_config {
                        info!("decoder config: {} bytes", config.len());
                    }
                    if let Some(file) = &mut recording {
                        file.write_all(&config).ok();
                    }
                    context.set_video_config_nals(config, codec);
                    sent_config = true;
                }
                if !sent_config {
                    warn!("dropping frame before the first IDR");
                    continue;
                }
                stats_frames += 1;
                stats_bytes += frame.nals.len();
                stats_encode += frame.encode_time;
                if let Some(file) = &mut recording {
                    file.write_all(&frame.nals).ok();
                }
                context.send_video_nal(frame.target_timestamp, frame.nals, frame.is_idr);

                if stats_started.elapsed() >= Duration::from_secs(5) {
                    let seconds = stats_started.elapsed().as_secs_f64();
                    let fps = stats_frames as f64 / seconds;
                    let mbps = stats_bytes as f64 * 8.0 / seconds / 1e6;
                    let encode_ms = stats_encode.as_secs_f64() * 1000.0 / stats_frames.max(1) as f64;
                    info!("encoded {fps:.1} fps, {mbps:.1} Mbps, encode {encode_ms:.1} ms");
                    println!("{}", serde_json::json!({ "event": "stats", "fps": fps, "mbps": mbps, "encode_ms": encode_ms }));
                    stats_started = Instant::now();
                    stats_frames = 0;
                    stats_bytes = 0;
                    stats_encode = Duration::ZERO;
                }
            }
        }
    });

    let mut frames: Option<SharedFrames> = None;
    let mut last_frame_id = 0u32;
    let mut last_discovery = Instant::now() - Duration::from_secs(10);
    let mut first_frame = true;
    let mut size_warned = false;
    let mut submit_stats_started = Instant::now();
    let mut submitted = 0u32;
    let mut copy_total = Duration::ZERO;

    while control.running.load(Ordering::Relaxed) {
        // Follow the newest published file: the game restarting creates a new one.
        if frames.as_ref().is_none_or(|f| !f.alive()) || last_discovery.elapsed() > Duration::from_secs(2) {
            last_discovery = Instant::now();
            let newest = shared_frames::discover().into_iter().next();
            let switch = match (&frames, &newest) {
                (Some(current), Some(path)) => current.path() != path || !current.alive(),
                (None, Some(_)) => true,
                (Some(_), None) => true,
                (None, None) => false,
            };
            if switch {
                frames = newest.and_then(|path| match SharedFrames::open(path.clone()) {
                    Ok(opened) => {
                        info!("reading frames from {}: {}x{} per eye, {:?}", path.display(), opened.eye_width, opened.eye_height, opened.format);
                        first_frame = true;
                        Some(opened)
                    }
                    Err(e) => {
                        if e.to_string() != "not published yet" {
                            warn!("cannot open {}: {e}", path.display());
                        }
                        None
                    }
                });
            }
        }

        let Some(shared) = &frames else {
            thread::sleep(Duration::from_millis(250));
            continue;
        };
        shared.heartbeat();
        if control.host_state_dirty.swap(false, Ordering::AcqRel) {
            let state = *control.host_state.lock();
            shared.write_host_state(&state);
        }

        if shared.eye_width < view_width || shared.eye_height < view_height {
            if !size_warned {
                error!(
                    "runtime publishes {}x{} per eye but the stream needs at least {}x{}; frames are not sent",
                    shared.eye_width, shared.eye_height, view_width, view_height
                );
                size_warned = true;
            }
            thread::sleep(Duration::from_millis(250));
            continue;
        }

        match shared.latest_frame_id() {
            Some(id) if id != last_frame_id => last_frame_id = id,
            _ => {
                thread::sleep(Duration::from_millis(1));
                continue;
            }
        }

        if let Some(params) = context.get_dynamic_encoder_params() {
            encoder.set_bitrate(params.bitrate_bps as u64);
        }

        let force_idr = control.idr_requested.swap(false, Ordering::Relaxed) || first_frame;
        let mut submitted_at_unix_ns = 0u64;
        let mut frame_id = 0u32;
        let mut copy_time = Duration::ZERO;
        // Stamp the frame with the tracking sample the game rendered it from;
        // the headset reprojects from that pose to the pose at display time.
        let mut target_timestamp = Duration::from_nanos(control.latest_tracking_ns.load(Ordering::Relaxed));
        let result = encoder.encode_with(force_idr, |out, stride| {
            let started = Instant::now();
            let copied = shared.copy_latest_bgra(out, width, height, stride);
            copy_time = started.elapsed();
            match copied {
                Some(info) => {
                    submitted_at_unix_ns = info.submitted_at_unix_ns;
                    frame_id = info.frame_id;
                    if info.tracking_timestamp_ns != 0 {
                        target_timestamp = Duration::from_nanos(info.tracking_timestamp_ns);
                    }
                    Some(target_timestamp)
                }
                None => None,
            }
        });
        match result {
            Ok(true) => {
                if first_frame {
                    let age_ms = wall_now_unix_ns().saturating_sub(submitted_at_unix_ns) as f64 / 1e6;
                    info!("first frame (game frame {frame_id}) submitted to the encoder {age_ms:.1} ms after the game submitted it");
                    first_frame = false;
                }
                context.report_present(target_timestamp, Duration::ZERO);
                submitted += 1;
                copy_total += copy_time;
                if submit_stats_started.elapsed() >= Duration::from_secs(5) {
                    info!(
                        "submitted {:.1} fps, copy into pixel buffer {:.1} ms, runtime dropped {} so far",
                        submitted as f64 / submit_stats_started.elapsed().as_secs_f64(),
                        copy_total.as_secs_f64() * 1000.0 / submitted.max(1) as f64,
                        shared.frames_dropped()
                    );
                    submit_stats_started = Instant::now();
                    submitted = 0;
                    copy_total = Duration::ZERO;
                }
            }
            Ok(false) => {
                // Torn read: the runtime was mid-write. The next poll gets it.
                control.idr_requested.fetch_or(force_idr, Ordering::Relaxed);
            }
            Err(e) => {
                error!("encode: {e}");
                thread::sleep(Duration::from_millis(100));
            }
        }
    }

    drop(encoder);
    sender_thread.join().ok();
}
