//! VideoToolbox encoder. Takes BGRA frames, produces Annex B NAL units in the
//! shape ALVR's client expects: one buffer holding the parameter sets
//! (VPS/SPS/PPS for HEVC, SPS/PPS for H.264) and one buffer per frame.

use alvr_common::anyhow::{bail, Result};
use alvr_session::CodecType;
use objc2_core_foundation::{CFBoolean, CFDictionary, CFNumber, CFRetained, CFString, CFType};
use objc2_core_media::{
    kCMSampleAttachmentKey_NotSync, kCMVideoCodecType_H264, kCMVideoCodecType_HEVC, CMFormatDescription,
    CMSampleBuffer, CMTime, CMVideoFormatDescriptionGetH264ParameterSetAtIndex,
    CMVideoFormatDescriptionGetHEVCParameterSetAtIndex,
};
use objc2_core_video::{
    kCVImageBufferColorPrimaries_ITU_R_709_2, kCVImageBufferTransferFunction_ITU_R_709_2,
    kCVImageBufferYCbCrMatrix_ITU_R_709_2, kCVPixelBufferHeightKey, kCVPixelBufferIOSurfacePropertiesKey, kCVPixelBufferPixelFormatTypeKey,
    kCVPixelBufferWidthKey, kCVPixelFormatType_32BGRA, CVPixelBuffer, CVPixelBufferGetBaseAddress,
    CVPixelBufferGetBytesPerRow, CVPixelBufferLockBaseAddress, CVPixelBufferLockFlags, CVPixelBufferPool,
    CVPixelBufferUnlockBaseAddress,
};
use objc2_video_toolbox::{
    kVTCompressionPropertyKey_AllowFrameReordering, kVTCompressionPropertyKey_AverageBitRate,
    kVTCompressionPropertyKey_ColorPrimaries, kVTCompressionPropertyKey_TransferFunction,
    kVTCompressionPropertyKey_YCbCrMatrix,
    kVTCompressionPropertyKey_ExpectedFrameRate, kVTCompressionPropertyKey_MaxFrameDelayCount,
    kVTCompressionPropertyKey_MaxKeyFrameInterval, kVTCompressionPropertyKey_PrioritizeEncodingSpeedOverQuality,
    kVTCompressionPropertyKey_ProfileLevel, kVTCompressionPropertyKey_RealTime, kVTEncodeFrameOptionKey_ForceKeyFrame,
    kVTProfileLevel_H264_High_AutoLevel, kVTProfileLevel_HEVC_Main_AutoLevel,
    kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder, VTCompressionSession, VTEncodeInfoFlags,
    VTSessionSetProperty,
};
use std::{
    ffi::c_void,
    ptr::{self, NonNull},
    sync::mpsc::{self, Receiver, Sender},
    time::{Duration, Instant},
};

const ANNEX_B_START_CODE: [u8; 4] = [0, 0, 0, 1];

pub struct EncodedFrame {
    /// The ALVR tracking timestamp this frame was rendered for.
    pub target_timestamp: Duration,
    pub is_idr: bool,
    /// Annex B NAL units for this frame, parameter sets excluded.
    pub nals: Vec<u8>,
    /// Annex B parameter sets, present when they changed (always on the first
    /// frame and after every IDR).
    pub config: Option<Vec<u8>>,
    pub encode_time: Duration,
}

/// Owned by the output callback through a raw pointer for the session's life.
struct CallbackContext {
    codec: CodecType,
    sender: Sender<EncodedFrame>,
}

/// Per frame, handed to VideoToolbox and back through the callback.
struct FrameContext {
    target_timestamp: Duration,
    submitted_at: Instant,
}

pub struct Encoder {
    session: CFRetained<VTCompressionSession>,
    pool: CFRetained<CVPixelBufferPool>,
    callback_context: *mut CallbackContext,
    height: u32,
    frame_index: i64,
    fps: f32,
    current_bitrate_bps: u64,
}

fn set_property(session: &VTCompressionSession, key: &CFString, value: &CFType) -> Result<()> {
    let status = unsafe { VTSessionSetProperty(session, key, Some(value)) };
    if status != 0 {
        bail!("VTSessionSetProperty({key}) failed: {status}");
    }
    Ok(())
}

impl Encoder {
    pub fn new(codec: CodecType, width: u32, height: u32, fps: f32, bitrate_bps: u64) -> Result<(Self, Receiver<EncodedFrame>)> {
        let codec_type = match codec {
            CodecType::H264 => kCMVideoCodecType_H264,
            CodecType::Hevc => kCMVideoCodecType_HEVC,
            CodecType::AV1 => bail!("AV1 is not supported by this encoder"),
        };

        let (sender, receiver) = mpsc::channel();
        let callback_context = Box::into_raw(Box::new(CallbackContext { codec, sender }));

        // Deliberately not kVTVideoEncoderSpecification_EnableLowLatencyRateControl:
        // on an M4 Max that mode capped the encoder at about 350 Mpx/s (41 fps
        // at 4096x2208) with 115 ms of queueing. Without it the same stream
        // encodes at 72 fps in 13 ms.
        let true_value: &CFType = CFBoolean::new(true);
        let spec_keys: [&CFString; 1] = [unsafe { kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder }];
        let specification: CFRetained<CFDictionary<CFString, CFType>> =
            CFDictionary::from_slices(&spec_keys, &[true_value]);

        let format_number = CFNumber::new_i32(kCVPixelFormatType_32BGRA as i32);
        let empty: CFRetained<CFDictionary<CFString, CFType>> = CFDictionary::from_slices(&[], &[]);
        let attribute_keys: [&CFString; 2] =
            [unsafe { kCVPixelBufferPixelFormatTypeKey }, unsafe { kCVPixelBufferIOSurfacePropertiesKey }];
        let attribute_values: [&CFType; 2] = [&format_number, &empty];
        let source_attributes: CFRetained<CFDictionary<CFString, CFType>> =
            CFDictionary::from_slices(&attribute_keys, &attribute_values);

        let mut session_ptr: *mut VTCompressionSession = ptr::null_mut();
        let status = unsafe {
            VTCompressionSession::create(
                None,
                width as i32,
                height as i32,
                codec_type,
                Some(specification.as_opaque()),
                Some(source_attributes.as_opaque()),
                None,
                Some(output_callback),
                callback_context as *mut c_void,
                NonNull::from(&mut session_ptr),
            )
        };
        if status != 0 || session_ptr.is_null() {
            unsafe { drop(Box::from_raw(callback_context)) };
            bail!("VTCompressionSessionCreate failed: {status}");
        }
        let session = unsafe { CFRetained::from_raw(NonNull::new_unchecked(session_ptr)) };

        let profile: &CFString = match codec {
            CodecType::H264 => unsafe { kVTProfileLevel_H264_High_AutoLevel },
            _ => unsafe { kVTProfileLevel_HEVC_Main_AutoLevel },
        };
        set_property(&session, unsafe { kVTCompressionPropertyKey_RealTime }, true_value)?;
        set_property(&session, unsafe { kVTCompressionPropertyKey_AllowFrameReordering }, CFBoolean::new(false))?;
        set_property(&session, unsafe { kVTCompressionPropertyKey_ProfileLevel }, profile)?;
        set_property(&session, unsafe { kVTCompressionPropertyKey_ExpectedFrameRate }, &CFNumber::new_f64(fps as f64))?;
        // Not supported by the Apple silicon hardware encoder; harmless to skip.
        set_property(&session, unsafe { kVTCompressionPropertyKey_MaxFrameDelayCount }, &CFNumber::new_i32(0)).ok();
        // ALVR asks for IDR frames itself; do not insert periodic ones.
        set_property(&session, unsafe { kVTCompressionPropertyKey_MaxKeyFrameInterval }, &CFNumber::new_i32(i32::MAX))?;
        set_property(&session, unsafe { kVTCompressionPropertyKey_AverageBitRate }, &CFNumber::new_i64(bitrate_bps as i64))?;
        // Optional on some hardware; a failure here is not fatal.
        if let Err(e) = set_property(&session, unsafe { kVTCompressionPropertyKey_PrioritizeEncodingSpeedOverQuality }, true_value) {
            alvr_common::warn!("{e}");
        }
        // The game renders sRGB; tag the stream as BT.709 so the decoder does
        // not guess.
        for (key, value) in [
            (unsafe { kVTCompressionPropertyKey_ColorPrimaries }, unsafe { kCVImageBufferColorPrimaries_ITU_R_709_2 }),
            (unsafe { kVTCompressionPropertyKey_TransferFunction }, unsafe { kCVImageBufferTransferFunction_ITU_R_709_2 }),
            (unsafe { kVTCompressionPropertyKey_YCbCrMatrix }, unsafe { kCVImageBufferYCbCrMatrix_ITU_R_709_2 }),
        ] {
            if let Err(e) = set_property(&session, key, value) {
                alvr_common::warn!("{e}");
            }
        }

        let mut pool_ptr: *mut CVPixelBufferPool = ptr::null_mut();
        let width_number = CFNumber::new_i32(width as i32);
        let height_number = CFNumber::new_i32(height as i32);
        let pool_keys: [&CFString; 4] = [
            unsafe { kCVPixelBufferPixelFormatTypeKey },
            unsafe { kCVPixelBufferWidthKey },
            unsafe { kCVPixelBufferHeightKey },
            unsafe { kCVPixelBufferIOSurfacePropertiesKey },
        ];
        let pool_values: [&CFType; 4] = [&format_number, &width_number, &height_number, &empty];
        let pool_attributes: CFRetained<CFDictionary<CFString, CFType>> =
            CFDictionary::from_slices(&pool_keys, &pool_values);
        let status = unsafe {
            CVPixelBufferPool::create(None, None, Some(pool_attributes.as_opaque()), NonNull::from(&mut pool_ptr))
        };
        if status != 0 || pool_ptr.is_null() {
            bail!("CVPixelBufferPoolCreate failed: {status}");
        }
        let pool = unsafe { CFRetained::from_raw(NonNull::new_unchecked(pool_ptr)) };

        Ok((
            Self {
                session,
                pool,
                callback_context,
                height,
                frame_index: 0,
                fps,
                current_bitrate_bps: bitrate_bps,
            },
            receiver,
        ))
    }

    pub fn set_bitrate(&mut self, bitrate_bps: u64) {
        if bitrate_bps == self.current_bitrate_bps || bitrate_bps == 0 {
            return;
        }
        if set_property(&self.session, unsafe { kVTCompressionPropertyKey_AverageBitRate }, &CFNumber::new_i64(bitrate_bps as i64)).is_ok() {
            self.current_bitrate_bps = bitrate_bps;
        }
    }

    /// Borrows a pooled BGRA buffer and lets `fill` write the pixels into it.
    /// `fill` receives the destination bytes and the row stride and returns the
    /// ALVR tracking timestamp to stamp the frame with, or None to skip it.
    pub fn encode_with(
        &mut self,
        force_idr: bool,
        fill: impl FnOnce(&mut [u8], usize) -> Option<Duration>,
    ) -> Result<bool> {
        let mut buffer_ptr: *mut CVPixelBuffer = ptr::null_mut();
        let status = unsafe { CVPixelBufferPool::create_pixel_buffer(None, &self.pool, NonNull::from(&mut buffer_ptr)) };
        if status != 0 || buffer_ptr.is_null() {
            bail!("CVPixelBufferPoolCreatePixelBuffer failed: {status}");
        }
        let buffer: CFRetained<CVPixelBuffer> = unsafe { CFRetained::from_raw(NonNull::new_unchecked(buffer_ptr)) };

        let filled = unsafe {
            CVPixelBufferLockBaseAddress(&buffer, CVPixelBufferLockFlags::empty());
            let base = CVPixelBufferGetBaseAddress(&buffer) as *mut u8;
            let stride = CVPixelBufferGetBytesPerRow(&buffer);
            let filled = if base.is_null() {
                None
            } else {
                fill(std::slice::from_raw_parts_mut(base, stride * self.height as usize), stride)
            };
            CVPixelBufferUnlockBaseAddress(&buffer, CVPixelBufferLockFlags::empty());
            filled
        };
        let Some(target_timestamp) = filled else {
            return Ok(false);
        };

        let frame_context = Box::into_raw(Box::new(FrameContext { target_timestamp, submitted_at: Instant::now() }));
        let timescale = 1_000_000;
        let presentation = unsafe {
            CMTime::new((self.frame_index as f64 * 1_000_000.0 / self.fps as f64) as i64, timescale)
        };
        let duration = unsafe { CMTime::new((1_000_000.0 / self.fps as f64) as i64, timescale) };
        self.frame_index += 1;

        let force_key: Option<CFRetained<CFDictionary<CFString, CFType>>> = force_idr.then(|| {
            let keys: [&CFString; 1] = [unsafe { kVTEncodeFrameOptionKey_ForceKeyFrame }];
            let values: [&CFType; 1] = [CFBoolean::new(true)];
            CFDictionary::from_slices(&keys, &values)
        });

        let mut flags = VTEncodeInfoFlags::empty();
        let status = unsafe {
            self.session.encode_frame(
                &buffer,
                presentation,
                duration,
                force_key.as_deref().map(|d| d.as_opaque()),
                frame_context as *mut c_void,
                &mut flags,
            )
        };
        if status != 0 {
            unsafe { drop(Box::from_raw(frame_context)) };
            bail!("VTCompressionSessionEncodeFrame failed: {status}");
        }
        Ok(true)
    }
}

impl Drop for Encoder {
    fn drop(&mut self) {
        unsafe {
            self.session.invalidate();
            // The callback cannot fire after invalidate returns.
            drop(Box::from_raw(self.callback_context));
        }
    }
}

/// Parameter sets from the format description as Annex B.
fn parameter_sets(description: &CMFormatDescription, codec: CodecType) -> Option<Vec<u8>> {
    let mut out = Vec::new();
    let mut count: usize = 0;
    let mut index = 0;
    loop {
        let mut pointer: *const u8 = ptr::null();
        let mut size: usize = 0;
        let mut header_length: i32 = 0;
        let status = unsafe {
            match codec {
                CodecType::H264 => CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
                    description, index, &mut pointer, &mut size, &mut count, &mut header_length,
                ),
                _ => CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(
                    description, index, &mut pointer, &mut size, &mut count, &mut header_length,
                ),
            }
        };
        if status != 0 || pointer.is_null() {
            return None;
        }
        out.extend_from_slice(&ANNEX_B_START_CODE);
        out.extend_from_slice(unsafe { std::slice::from_raw_parts(pointer, size) });
        index += 1;
        if index >= count {
            break;
        }
    }
    Some(out)
}

/// Rewrites the AVCC/HVCC length-prefixed sample into Annex B start codes.
fn annex_b_from_sample(sample: &CMSampleBuffer) -> Option<Vec<u8>> {
    let block = unsafe { sample.data_buffer()? };
    let mut length_at_offset: usize = 0;
    let mut total_length: usize = 0;
    let mut data: *mut i8 = ptr::null_mut();
    let status = unsafe { block.data_pointer(0, &mut length_at_offset, &mut total_length, &mut data) };
    if status != 0 || data.is_null() || length_at_offset != total_length {
        // Non-contiguous block buffers are not something VideoToolbox produces
        // for encoded frames in practice; refuse rather than mis-parse.
        return None;
    }
    let bytes = unsafe { std::slice::from_raw_parts(data as *const u8, total_length) };

    let mut out = Vec::with_capacity(total_length + 16);
    let mut offset = 0;
    while offset + 4 <= bytes.len() {
        let length = u32::from_be_bytes(bytes[offset..offset + 4].try_into().unwrap()) as usize;
        offset += 4;
        if offset + length > bytes.len() {
            return None;
        }
        out.extend_from_slice(&ANNEX_B_START_CODE);
        out.extend_from_slice(&bytes[offset..offset + length]);
        offset += length;
    }
    Some(out)
}

fn is_sync_sample(sample: &CMSampleBuffer) -> bool {
    let Some(attachments) = (unsafe { sample.sample_attachments_array(false) }) else {
        return true;
    };
    if attachments.count() == 0 {
        return true;
    }
    let first = unsafe { attachments.value_at_index(0) } as *const CFDictionary;
    if first.is_null() {
        return true;
    }
    let not_sync = unsafe { (*first).value(kCMSampleAttachmentKey_NotSync as *const CFString as *const c_void) }
        as *const CFBoolean;
    if not_sync.is_null() {
        return true;
    }
    !unsafe { (*not_sync).as_bool() }
}

unsafe extern "C-unwind" fn output_callback(
    refcon: *mut c_void,
    frame_refcon: *mut c_void,
    status: i32,
    _flags: VTEncodeInfoFlags,
    sample: *mut CMSampleBuffer,
) {
    let context = &*(refcon as *const CallbackContext);
    let frame = Box::from_raw(frame_refcon as *mut FrameContext);
    if status != 0 || sample.is_null() {
        alvr_common::error!("encoder output failed: {status}");
        return;
    }
    let sample = &*sample;
    let Some(nals) = annex_b_from_sample(sample) else {
        alvr_common::error!("could not read encoded sample");
        return;
    };
    let is_idr = is_sync_sample(sample);
    let config = if is_idr {
        sample.format_description().and_then(|description| parameter_sets(&description, context.codec))
    } else {
        None
    };
    context
        .sender
        .send(EncodedFrame {
            target_timestamp: frame.target_timestamp,
            is_idr,
            nals,
            config,
            encode_time: frame.submitted_at.elapsed(),
        })
        .ok();
}
