//! Reads the frames the OpenXR runtime publishes from inside the CrossOver
//! bottle. Mirrors bridge/include/wheelio_vr_shared.hpp byte for byte; the
//! offsets below are that header's layout.

use alvr_common::anyhow::{bail, Result};
use memmap2::MmapMut;
use std::{
    fs::OpenOptions,
    path::PathBuf,
    sync::atomic::{fence, AtomicU32, AtomicU64, Ordering},
};

const MAGIC: u32 = 0x5256_5748; // "HWVR"
const VERSION: u32 = 3;
const SLOT_COUNT: u32 = 3;
const FIRST_SLOT_OFFSET: u64 = 4096;
const SLOT_HEADER_SIZE: u64 = 144;
const HOST_STATE_OFFSET: usize = 256;
const MAX_EYE_SIZE: u32 = 8192;

const FORMAT_RGBA8: u32 = 1;
const FORMAT_BGRA8: u32 = 2;

// SharedHeader field offsets.
const H_MAGIC: usize = 0;
const H_VERSION: usize = 4;
const H_SLOT_COUNT: usize = 8;
const H_FORMAT: usize = 12;
const H_EYE_WIDTH: usize = 16;
const H_EYE_HEIGHT: usize = 20;
const H_EYE_STRIDE: usize = 24;
const H_EYE_SIZE: usize = 28;
const H_SLOT_STRIDE: usize = 32;
const H_FIRST_SLOT_OFFSET: usize = 40;
const H_TOTAL_SIZE: usize = 48;
const H_LATEST_SLOT: usize = 56;
const H_FRAMES_DROPPED: usize = 72;
const H_READER_HEARTBEAT: usize = 80;

// SlotHeader field offsets.
const S_SEQUENCE: usize = 0;
const S_FRAME_ID: usize = 4;
const S_SUBMITTED_AT_UNIX_NS: usize = 16;
const S_TRACKING_TIMESTAMP_NS: usize = 136;

// HostState field offsets (relative to HOST_STATE_OFFSET).
const HS_SEQUENCE: usize = 0;
const HS_RECENTER_GENERATION: usize = 4;
const HS_TRACKING_TIMESTAMP_NS: usize = 8;
const HS_WRITTEN_AT_UNIX_NS: usize = 16;
const HS_HEAD_ORIENTATION: usize = 24;
const HS_HEAD_POSITION: usize = 40;
const HS_HAS_POSE: usize = 52;
const HS_LINEAR_VELOCITY: usize = 56;
const HS_ANGULAR_VELOCITY: usize = 72;
const HS_FOV: usize = 88;
const HS_IPD: usize = 120;
const HS_HAS_VIEWS: usize = 124;
const HOST_STATE_SIZE: usize = 128;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PixelFormat {
    Rgba8,
    Bgra8,
}

pub struct FrameInfo {
    pub frame_id: u32,
    pub submitted_at_unix_ns: u64,
    /// ALVR sample timestamp of the pose this frame was rendered with; zero
    /// before tracking reached the runtime.
    pub tracking_timestamp_ns: u64,
}

/// What the streamer tells the runtime. Mirrors wheelio_vr::HostState.
#[derive(Clone, Copy, Default, Debug)]
pub struct HostState {
    pub recenter_generation: u32,
    pub tracking_timestamp_ns: u64,
    pub head_orientation: [f32; 4],
    pub head_position: [f32; 3],
    pub has_pose: bool,
    pub linear_velocity: [f32; 3],
    pub angular_velocity: [f32; 3],
    pub fov: [[f32; 4]; 2],
    pub ipd_m: f32,
    pub has_views: bool,
}

pub struct SharedFrames {
    mapping: MmapMut,
    path: PathBuf,
    pub eye_width: u32,
    pub eye_height: u32,
    pub eye_stride: usize,
    eye_size: usize,
    slot_stride: usize,
    pub format: PixelFormat,
}

fn read_u32(bytes: &[u8], offset: usize) -> u32 {
    u32::from_le_bytes(bytes[offset..offset + 4].try_into().unwrap())
}

fn read_u64(bytes: &[u8], offset: usize) -> u64 {
    u64::from_le_bytes(bytes[offset..offset + 8].try_into().unwrap())
}

/// Every place a bottle could hold a published buffer, newest first.
pub fn discover() -> Vec<PathBuf> {
    let Some(home) = dirs::home_dir() else {
        return vec![];
    };
    let pattern = home
        .join("Library/Application Support/CrossOver/Bottles/*/drive_c/users/*/AppData/Local/Wheelio/wheelio_vr_*.bin")
        .to_string_lossy()
        .into_owned();
    let Ok(paths) = glob::glob(&pattern) else {
        return vec![];
    };
    let mut found: Vec<(std::time::SystemTime, PathBuf)> = paths
        .flatten()
        .filter_map(|path| {
            let modified = path.metadata().ok()?.modified().ok()?;
            Some((modified, path))
        })
        .collect();
    found.sort_by(|a, b| b.0.cmp(&a.0));
    found.into_iter().map(|(_, path)| path).collect()
}

impl SharedFrames {
    pub fn open(path: PathBuf) -> Result<Self> {
        let file = OpenOptions::new().read(true).write(true).open(&path)?;
        let mapping = unsafe { MmapMut::map_mut(&file)? };
        if mapping.len() < 96 {
            bail!("shared file too small");
        }

        let bytes = &mapping[..];
        if read_u32(bytes, H_MAGIC) == 0 {
            // The runtime writes the magic last; the file exists but is not
            // ready yet.
            bail!("not published yet");
        }
        let eye_width = read_u32(bytes, H_EYE_WIDTH);
        let eye_height = read_u32(bytes, H_EYE_HEIGHT);
        let eye_stride = read_u32(bytes, H_EYE_STRIDE);
        let eye_size = read_u32(bytes, H_EYE_SIZE);
        let slot_stride = read_u64(bytes, H_SLOT_STRIDE);
        let total_size = read_u64(bytes, H_TOTAL_SIZE);

        // Trust nothing in the header until the geometry adds up to the file
        // we actually mapped.
        let sane = read_u32(bytes, H_MAGIC) == MAGIC
            && read_u32(bytes, H_VERSION) == VERSION
            && read_u32(bytes, H_SLOT_COUNT) == SLOT_COUNT
            && (1..=MAX_EYE_SIZE).contains(&eye_width)
            && (1..=MAX_EYE_SIZE).contains(&eye_height)
            && eye_stride == eye_width * 4
            && eye_size == eye_stride * eye_height
            && read_u64(bytes, H_FIRST_SLOT_OFFSET) == FIRST_SLOT_OFFSET
            && slot_stride == SLOT_HEADER_SIZE + 2 * eye_size as u64
            && total_size == FIRST_SLOT_OFFSET + SLOT_COUNT as u64 * slot_stride
            && total_size <= mapping.len() as u64;
        if !sane {
            bail!("shared frame header failed validation");
        }

        let format = match read_u32(bytes, H_FORMAT) {
            FORMAT_RGBA8 => PixelFormat::Rgba8,
            FORMAT_BGRA8 => PixelFormat::Bgra8,
            other => bail!("unsupported shared pixel format {other}"),
        };

        Ok(Self {
            mapping,
            path,
            eye_width,
            eye_height,
            eye_stride: eye_stride as usize,
            eye_size: eye_size as usize,
            slot_stride: slot_stride as usize,
            format,
        })
    }

    pub fn path(&self) -> &PathBuf {
        &self.path
    }

    fn atomic_u32(&self, offset: usize) -> &AtomicU32 {
        unsafe { &*(self.mapping.as_ptr().add(offset) as *const AtomicU32) }
    }

    fn atomic_u64(&self, offset: usize) -> &AtomicU64 {
        unsafe { &*(self.mapping.as_ptr().add(offset) as *const AtomicU64) }
    }

    /// The runtime stops reading frames back once this stops changing.
    pub fn heartbeat(&self) {
        self.atomic_u64(H_READER_HEARTBEAT).fetch_add(1, Ordering::Release);
    }

    /// Frames the runtime could not publish (GPU copy never landed).
    pub fn frames_dropped(&self) -> u64 {
        self.atomic_u64(H_FRAMES_DROPPED).load(Ordering::Relaxed)
    }

    /// False once the game has shut down cleanly (it clears the magic).
    pub fn alive(&self) -> bool {
        self.atomic_u32(H_MAGIC).load(Ordering::Acquire) == MAGIC
    }

    fn latest_slot_offset(&self) -> Option<usize> {
        let index = self.atomic_u32(H_LATEST_SLOT).load(Ordering::Acquire) as i32;
        if index < 0 || index as u32 >= SLOT_COUNT {
            return None;
        }
        Some(FIRST_SLOT_OFFSET as usize + index as usize * self.slot_stride)
    }

    /// Frame id of the newest published slot, without copying anything.
    pub fn latest_frame_id(&self) -> Option<u32> {
        let slot = self.latest_slot_offset()?;
        Some(read_u32(&self.mapping[..], slot + S_FRAME_ID))
    }

    /// Copies the newest frame into `out` as BGRA, both eyes side by side, each
    /// eye centre-cropped to `out_width / 2` by `out_height`. Returns None when
    /// a write was in progress or the slot changed under us.
    pub fn copy_latest_bgra(&self, out: &mut [u8], out_width: u32, out_height: u32, out_stride: usize) -> Option<FrameInfo> {
        let slot = self.latest_slot_offset()?;
        let bytes = &self.mapping[..];

        let sequence = self.atomic_u32(slot + S_SEQUENCE).load(Ordering::Acquire);
        if sequence % 2 != 0 {
            return None;
        }

        let eye_out_width = out_width / 2;
        if eye_out_width > self.eye_width || out_height > self.eye_height {
            return None;
        }
        let crop_x = ((self.eye_width - eye_out_width) / 2) as usize * 4;
        let crop_y = ((self.eye_height - out_height) / 2) as usize;
        let row_bytes = eye_out_width as usize * 4;

        let pixels = slot + SLOT_HEADER_SIZE as usize;
        for eye in 0..2usize {
            let eye_base = pixels + eye * self.eye_size;
            for y in 0..out_height as usize {
                let src = eye_base + (crop_y + y) * self.eye_stride + crop_x;
                let dst = y * out_stride + eye * row_bytes;
                let source_row = &bytes[src..src + row_bytes];
                let destination_row = &mut out[dst..dst + row_bytes];
                match self.format {
                    PixelFormat::Bgra8 => destination_row.copy_from_slice(source_row),
                    PixelFormat::Rgba8 => {
                        for (d, s) in destination_row.chunks_exact_mut(4).zip(source_row.chunks_exact(4)) {
                            d[0] = s[2];
                            d[1] = s[1];
                            d[2] = s[0];
                            d[3] = s[3];
                        }
                    }
                }
            }
        }

        fence(Ordering::Acquire);
        if self.atomic_u32(slot + S_SEQUENCE).load(Ordering::Acquire) != sequence {
            return None;
        }

        Some(FrameInfo {
            frame_id: read_u32(bytes, slot + S_FRAME_ID),
            submitted_at_unix_ns: read_u64(bytes, slot + S_SUBMITTED_AT_UNIX_NS),
            tracking_timestamp_ns: read_u64(bytes, slot + S_TRACKING_TIMESTAMP_NS),
        })
    }

    /// Publishes the head pose and optics for the runtime. Seqlocked: the
    /// runtime retries if it catches us mid-write. The mapping is shared, so
    /// the write goes through raw pointers rather than the slice.
    pub fn write_host_state(&self, state: &HostState) {
        let base = self.mapping.as_ptr() as *mut u8;
        let at = |offset: usize| unsafe { base.add(HOST_STATE_OFFSET + offset) };
        let write_u32 = |offset: usize, v: u32| unsafe { at(offset).cast::<u32>().write_unaligned(v) };
        let write_u64 = |offset: usize, v: u64| unsafe { at(offset).cast::<u64>().write_unaligned(v) };
        let write_f32s = |offset: usize, v: &[f32]| {
            for (i, x) in v.iter().enumerate() {
                unsafe { at(offset + i * 4).cast::<f32>().write_unaligned(*x) };
            }
        };
        debug_assert!(HOST_STATE_OFFSET + HOST_STATE_SIZE <= 4096);

        let sequence = self.atomic_u32(HOST_STATE_OFFSET + HS_SEQUENCE);
        let before = sequence.load(Ordering::Relaxed);
        sequence.store(before + 1, Ordering::Release);
        fence(Ordering::Release);

        write_u32(HS_RECENTER_GENERATION, state.recenter_generation);
        write_u64(HS_TRACKING_TIMESTAMP_NS, state.tracking_timestamp_ns);
        write_u64(
            HS_WRITTEN_AT_UNIX_NS,
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .map(|d| d.as_nanos() as u64)
                .unwrap_or(0),
        );
        write_f32s(HS_HEAD_ORIENTATION, &state.head_orientation);
        write_f32s(HS_HEAD_POSITION, &state.head_position);
        write_f32s(HS_HAS_POSE, &[if state.has_pose { 1.0 } else { 0.0 }]);
        write_f32s(HS_LINEAR_VELOCITY, &state.linear_velocity);
        write_f32s(HS_ANGULAR_VELOCITY, &state.angular_velocity);
        write_f32s(HS_FOV, &state.fov[0]);
        write_f32s(HS_FOV + 16, &state.fov[1]);
        write_f32s(HS_IPD, &[state.ipd_m]);
        write_u32(HS_HAS_VIEWS, state.has_views as u32);

        fence(Ordering::Release);
        sequence.store(before + 2, Ordering::Release);
    }
}

/// Saves the headset's optics next to the shared frame files, where the
/// OpenXR runtime reads them at start-up. The game fixes its FOV from its
/// first frames, so the runtime must know the headset before it connects.
pub fn save_headset_optics(fov: &[alvr_common::Fov; 2], ipd_m: f32) {
    let Some(newest) = discover().into_iter().next() else {
        alvr_common::warn!("no shared frame file yet; headset optics not saved");
        return;
    };
    let Some(directory) = newest.parent() else { return };
    let contents = format!(
        "{} {} {} {}\n{} {} {} {}\n{}\n",
        fov[0].left, fov[0].right, fov[0].up, fov[0].down, fov[1].left, fov[1].right, fov[1].up, fov[1].down, ipd_m
    );
    let path = directory.join("headset_optics.txt");
    match std::fs::write(&path, contents) {
        Ok(()) => alvr_common::info!("saved headset optics to {}", path.display()),
        Err(e) => alvr_common::warn!("cannot save headset optics to {}: {e}", path.display()),
    }
}
