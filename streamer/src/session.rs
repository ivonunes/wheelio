//! Our ALVR session overrides. ALVR keeps every setting in session.json; the
//! dashboard we don't ship would normally edit it. We patch the handful of
//! values that differ for this streamer before the core reads the file.

use alvr_common::anyhow::Result;
use alvr_session::CodecType;
use alvr_packets::{PathSegment, PathValuePair};
use alvr_server_io::ServerSessionManager;
use serde_json::json;
use std::path::Path;

fn setting(path: &str, value: serde_json::Value) -> PathValuePair {
    PathValuePair {
        path: path
            .split('/')
            .map(|segment| PathSegment::Name(segment.to_owned()))
            .collect(),
        value,
    }
}

/// Eye size we stream. The OpenXR runtime renders at this size too, so no
/// scaling happens between the game and the encoder. Multiples of 32 because
/// ALVR aligns the negotiated resolution down to 32.
pub const VIEW_WIDTH: u32 = 2048;
pub const VIEW_HEIGHT: u32 = 2208;

/// What the host app chooses for the user.
pub struct Options {
    pub codec: CodecType,
    pub bitrate_mbps: u64,
    pub mute_mac: bool,
}

impl Default for Options {
    fn default() -> Self {
        Self { codec: CodecType::Hevc, bitrate_mbps: 30, mute_mac: true }
    }
}

/// `--codec hevc|h264 --bitrate <Mbps> --mute 0|1`, all optional.
pub fn parse_options(args: impl Iterator<Item = String>) -> Options {
    let mut options = Options::default();
    let mut args = args.peekable();
    while let Some(arg) = args.next() {
        let value = args.next();
        match (arg.as_str(), value.as_deref()) {
            ("--codec", Some("h264")) => options.codec = CodecType::H264,
            ("--codec", Some("hevc")) => options.codec = CodecType::Hevc,
            ("--bitrate", Some(v)) => {
                if let Ok(mbps) = v.parse::<u64>() {
                    options.bitrate_mbps = mbps.clamp(5, 200);
                }
            }
            ("--mute", Some(v)) => options.mute_mac = v == "1",
            _ => {}
        }
    }
    options
}

pub fn apply_overrides(session_path: &Path, options: &Options) -> Result<()> {
    let codec = match options.codec {
        CodecType::H264 => "H264",
        _ => "Hevc",
    };
    let mut manager = ServerSessionManager::new(Some(session_path.to_owned()));
    manager.set_values(vec![
        setting("session_settings/video/preferred_codec/variant", json!(codec)),
        setting("session_settings/video/bitrate/mode/variant", json!("ConstantMbps")),
        setting("session_settings/video/bitrate/mode/ConstantMbps", json!(options.bitrate_mbps)),
        setting("session_settings/audio/game_audio/content/mute_when_streaming", json!(options.mute_mac)),
        // Foveated encoding needs the server to pre-warp the image the way the
        // SteamVR driver's shader does. We send the plain frame.
        setting("session_settings/video/foveated_encoding/enabled", json!(false)),
        setting("session_settings/video/transcoding_view_resolution/variant", json!("Absolute")),
        setting("session_settings/video/transcoding_view_resolution/Absolute/width", json!(VIEW_WIDTH)),
        setting("session_settings/video/transcoding_view_resolution/Absolute/height/set", json!(true)),
        setting(
            "session_settings/video/transcoding_view_resolution/Absolute/height/content",
            json!(VIEW_HEIGHT),
        ),
        setting("session_settings/video/emulated_headset_view_resolution/variant", json!("Absolute")),
        setting("session_settings/video/emulated_headset_view_resolution/Absolute/width", json!(VIEW_WIDTH)),
        setting("session_settings/video/emulated_headset_view_resolution/Absolute/height/set", json!(true)),
        setting(
            "session_settings/video/emulated_headset_view_resolution/Absolute/height/content",
            json!(VIEW_HEIGHT),
        ),
        // VideoToolbox converts our RGB frames to video-range YCbCr; tell the
        // client so it does not stretch the levels.
        setting("session_settings/video/encoder_config/server_overrides_use_full_range", json!(true)),
        setting("session_settings/video/encoder_config/use_full_range", json!(false)),
        // Game audio comes from the system tap in audio.rs, exposed as a device
        // ALVR can find by name. No microphone, and no controllers: the wheel is
        // the input.
        setting("session_settings/audio/game_audio/enabled", json!(true)),
        setting("session_settings/audio/game_audio/content/device/set", json!(true)),
        setting("session_settings/audio/game_audio/content/device/content/variant", json!("NameSubstring")),
        setting("session_settings/audio/game_audio/content/device/content/NameSubstring", json!(crate::audio::DEVICE_NAME)),
        setting("session_settings/audio/microphone/enabled", json!(false)),
        setting("session_settings/headset/controllers/enabled", json!(false)),
        // Without the dashboard there is nobody to click "Trust".
        setting("session_settings/connection/client_discovery/content/auto_trust_clients", json!(true)),
        setting("session_settings/extra/logging/log_to_disk", json!(true)),
    ])
}

#[cfg(test)]
mod tests {
    use super::*;

    fn parse(args: &[&str]) -> Options {
        parse_options(args.iter().map(|a| a.to_string()))
    }

    #[test]
    fn defaults_when_no_arguments() {
        let options = parse(&[]);
        assert!(matches!(options.codec, CodecType::Hevc));
        assert_eq!(options.bitrate_mbps, 30);
        assert!(options.mute_mac);
    }

    #[test]
    fn parses_every_option() {
        let options = parse(&["--codec", "h264", "--bitrate", "80", "--mute", "0"]);
        assert!(matches!(options.codec, CodecType::H264));
        assert_eq!(options.bitrate_mbps, 80);
        assert!(!options.mute_mac);
    }

    #[test]
    fn clamps_bitrate_and_ignores_junk() {
        let options = parse(&["--bitrate", "1", "--codec", "av1", "--bogus"]);
        assert_eq!(options.bitrate_mbps, 5);
        assert!(matches!(options.codec, CodecType::Hevc));
        assert_eq!(parse(&["--bitrate", "999"]).bitrate_mbps, 200);
    }
}
