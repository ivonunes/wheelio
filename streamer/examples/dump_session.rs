// Prints the default ALVR session JSON so the shape of the fields we override
// can be checked against the real schema.
fn main() {
    let session = alvr_session::SessionConfig::default();
    let json = serde_json::to_value(&session).unwrap();
    let s = &json["session_settings"];
    for path in [
        "video/preferred_codec",
        "video/foveated_encoding/enabled",
        "video/transcoding_view_resolution",
        "video/bitrate/mode",
        "audio/game_audio/enabled",
        "audio/microphone/enabled",
        "headset/controllers/enabled",
        "connection/client_discovery/content/auto_trust_clients",
        "connection/stream_protocol",
        "extra/logging/log_to_disk",
    ] {
        let mut v = s;
        for seg in path.split('/') {
            v = &v[seg];
        }
        println!("{path} = {}", serde_json::to_string(v).unwrap());
    }
}
