// Reads ALVR's statistics from the streamer's local events socket (what the
// ALVR dashboard would show) and prints one line per summary. Run while a
// headset is streaming: cargo run --release --example alvr_stats -- 30
use futures_util::StreamExt;
use tokio_tungstenite::tungstenite::{client::IntoClientRequest, Message};

#[tokio::main]
async fn main() {
    let seconds: u64 = std::env::args().nth(1).and_then(|s| s.parse().ok()).unwrap_or(20);
    let mut request = "ws://127.0.0.1:8082/api/events".into_client_request().unwrap();
    request.headers_mut().insert("X-ALVR", "true".parse().unwrap());
    let (stream, _) = tokio_tungstenite::connect_async(request).await.expect("streamer not running");
    let (_, mut read) = stream.split();

    let deadline = tokio::time::Instant::now() + std::time::Duration::from_secs(seconds);
    let mut summaries: Vec<serde_json::Value> = Vec::new();
    let mut raw_shown = 0;
    while let Ok(Some(Ok(message))) = tokio::time::timeout_at(deadline, read.next()).await {
        if std::env::var_os("ALVR_STATS_RAW").is_some() && raw_shown < 5 {
            raw_shown += 1;
            let preview: String = format!("{message:?}").chars().take(240).collect();
            eprintln!("raw: {preview}");
        }
        let Message::Text(text) = message else { continue };
        let Ok(event) = serde_json::from_str::<serde_json::Value>(&text) else { continue };
        let event_type = &event["event_type"];
        if event_type["id"] == "StatisticsSummary" {
            let summary = &event_type["data"];
            println!(
                "total {:>5.1} ms  network {:>5.1}  encode {:>5.1}  decode {:>5.1}  client {} fps  server {} fps  {:.1} Mbit/s  lost/s {}",
                summary["total_latency_ms"].as_f64().unwrap_or(0.0),
                summary["network_latency_ms"].as_f64().unwrap_or(0.0),
                summary["encode_latency_ms"].as_f64().unwrap_or(0.0),
                summary["decode_latency_ms"].as_f64().unwrap_or(0.0),
                summary["client_fps"],
                summary["server_fps"],
                summary["video_mbits_per_sec"].as_f64().unwrap_or(0.0),
                summary["packets_lost_per_sec"]
            );
            summaries.push(summary.clone());
        }
    }
    if !summaries.is_empty() {
        let avg = |key: &str| {
            summaries.iter().filter_map(|s| s[key].as_f64()).sum::<f64>() / summaries.len() as f64
        };
        println!(
            "\naverage over {} samples: total {:.1} ms, network {:.1} ms, encode {:.1} ms, decode {:.1} ms",
            summaries.len(),
            avg("total_latency_ms"),
            avg("network_latency_ms"),
            avg("encode_latency_ms"),
            avg("decode_latency_ms")
        );
    } else {
        println!("no statistics received: is a headset connected?");
    }
}
