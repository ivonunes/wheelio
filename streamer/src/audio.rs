//! Game audio capture. macOS has no loopback device, so ALVR's stock audio
//! path (record from an output device) fails here. Instead we create a Core
//! Audio process tap of everything this Mac plays except ourselves, and wrap
//! it in a private aggregate device named "Wheelio Game Audio". ALVR then
//! records from that device by name through its unchanged cpal path.
//!
//! Needs macOS 14.2 or later and the System Audio Recording permission; the
//! first run prompts for it.

use alvr_common::anyhow::{bail, Result};
use objc2::{rc::Retained, AnyThread};
use objc2_core_audio::{
    kAudioAggregateDeviceIsPrivateKey, kAudioAggregateDeviceMainSubDeviceKey, kAudioAggregateDeviceNameKey,
    kAudioAggregateDeviceSubDeviceListKey, kAudioAggregateDeviceTapAutoStartKey, kAudioAggregateDeviceTapListKey,
    kAudioAggregateDeviceUIDKey, kAudioDevicePropertyDeviceUID, kAudioDevicePropertyStreams,
    kAudioDevicePropertyTransportType, kAudioDeviceTransportTypeBuiltIn, kAudioHardwarePropertyDevices,
    kAudioObjectPropertyElementMain, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyScopeInput,
    kAudioObjectPropertyScopeOutput, kAudioObjectSystemObject, kAudioSubDeviceUIDKey, kAudioSubTapUIDKey,
    AudioHardwareCreateAggregateDevice, AudioHardwareCreateProcessTap, AudioHardwareDestroyAggregateDevice,
    AudioHardwareDestroyProcessTap, AudioObjectGetPropertyData, AudioObjectGetPropertyDataSize, AudioObjectID,
    AudioObjectPropertyAddress, CATapDescription, CATapMuteBehavior,
};
use objc2_core_foundation::{CFArray, CFDictionary, CFNumber, CFRetained, CFString, CFType};
use objc2_foundation::{NSArray, NSNumber, NSString};
use std::{ffi::CStr, ptr::NonNull};

/// The name ALVR's game audio device setting points at (substring match).
pub const DEVICE_NAME: &str = "Wheelio Game Audio";
const DEVICE_UID: &str = "uk.ivonunes.wheelio.game-audio";
// kAudioSubTapDriftCompensationKey from AudioHardware.h; missing from the bindings.
const SUB_TAP_DRIFT_COMPENSATION_KEY: &CStr = c"drift";

pub struct GameAudioCapture {
    tap_id: AudioObjectID,
    aggregate_id: AudioObjectID,
}

fn key(name: &CStr) -> CFRetained<CFString> {
    CFString::from_str(name.to_str().expect("Core Audio key is ASCII"))
}

fn get_property<T: Copy>(object: AudioObjectID, selector: u32, scope: u32, out: &mut T) -> Result<()> {
    let mut address = AudioObjectPropertyAddress { mSelector: selector, mScope: scope, mElement: kAudioObjectPropertyElementMain };
    let mut size = std::mem::size_of::<T>() as u32;
    let status = unsafe {
        AudioObjectGetPropertyData(
            object,
            NonNull::from(&mut address),
            0,
            std::ptr::null(),
            NonNull::from(&mut size),
            NonNull::from(out).cast(),
        )
    };
    if status != 0 {
        bail!("AudioObjectGetPropertyData({selector:#x}) failed ({status})");
    }
    Ok(())
}

fn property_count<T>(object: AudioObjectID, selector: u32, scope: u32) -> usize {
    let mut address = AudioObjectPropertyAddress { mSelector: selector, mScope: scope, mElement: kAudioObjectPropertyElementMain };
    let mut size: u32 = 0;
    let status = unsafe {
        AudioObjectGetPropertyDataSize(object, NonNull::from(&mut address), 0, std::ptr::null(), NonNull::from(&mut size))
    };
    if status != 0 {
        return 0;
    }
    size as usize / std::mem::size_of::<T>()
}

fn device_uid(device: AudioObjectID) -> Result<CFRetained<CFString>> {
    let mut uid: *const CFString = std::ptr::null();
    get_property(device, kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal, &mut uid)?;
    if uid.is_null() {
        bail!("device {device} has no UID");
    }
    // The property hands back a +1 reference.
    Ok(unsafe { CFRetained::from_raw(NonNull::new_unchecked(uid as *mut CFString)) })
}

/// A device with output streams and no input streams. The aggregate needs an
/// output sub-device to be listed as an output device, but any input streams
/// it brought along would be captured too and would make macOS ask for
/// microphone access. Built-in speakers are the usual answer.
fn output_only_device_uid() -> Result<CFRetained<CFString>> {
    let system = kAudioObjectSystemObject as AudioObjectID;
    let count = property_count::<AudioObjectID>(system, kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal);
    let mut devices = vec![0 as AudioObjectID; count];
    let mut address = AudioObjectPropertyAddress {
        mSelector: kAudioHardwarePropertyDevices,
        mScope: kAudioObjectPropertyScopeGlobal,
        mElement: kAudioObjectPropertyElementMain,
    };
    let mut size = (count * std::mem::size_of::<AudioObjectID>()) as u32;
    let status = unsafe {
        AudioObjectGetPropertyData(
            system,
            NonNull::from(&mut address),
            0,
            std::ptr::null(),
            NonNull::from(&mut size),
            NonNull::new(devices.as_mut_ptr()).unwrap().cast(),
        )
    };
    if status != 0 {
        bail!("cannot list audio devices ({status})");
    }

    let mut candidates: Vec<(bool, AudioObjectID)> = devices
        .into_iter()
        .filter(|&device| {
            property_count::<AudioObjectID>(device, kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput) > 0
                && property_count::<AudioObjectID>(device, kAudioDevicePropertyStreams, kAudioObjectPropertyScopeInput) == 0
        })
        .map(|device| {
            let mut transport: u32 = 0;
            get_property(device, kAudioDevicePropertyTransportType, kAudioObjectPropertyScopeGlobal, &mut transport).ok();
            (transport == kAudioDeviceTransportTypeBuiltIn, device)
        })
        .collect();
    // Built-in first: it is always there and never carries a microphone.
    candidates.sort_by_key(|(built_in, _)| !*built_in);
    let Some((_, device)) = candidates.first() else {
        bail!("no output-only audio device to anchor the capture device on");
    };
    device_uid(*device)
}

impl GameAudioCapture {
    /// `mute_locally` silences the Mac's own output while the tap is being
    /// read, so the game is heard in the headset only.
    pub fn start(mute_locally: bool) -> Result<Self> {
        // The recording side opens only the aggregate's first input stream, so
        // the tap must be the only one: anchor on a device with no inputs.
        let output_uid = output_only_device_uid()?;

        // Tap everything: with a headset on, the only sound worth hearing is the
        // game's, and this process plays none itself. (The exclusion list takes
        // Core Audio process objects, not PIDs.)
        let excluded: Retained<NSArray<NSNumber>> = NSArray::from_slice(&[]);
        let description: Retained<CATapDescription> =
            unsafe { CATapDescription::initStereoGlobalTapButExcludeProcesses(CATapDescription::alloc(), &excluded) };
        unsafe {
            description.setName(&NSString::from_str(DEVICE_NAME));
            description.setMuteBehavior(if mute_locally {
                CATapMuteBehavior::MutedWhenTapped
            } else {
                CATapMuteBehavior::Unmuted
            });
        }
        let tap_uid = unsafe { description.UUID().UUIDString() }.to_string();

        let mut tap_id: AudioObjectID = 0;
        let status = unsafe { AudioHardwareCreateProcessTap(Some(&description), &mut tap_id) };
        if status != 0 || tap_id == 0 {
            bail!("AudioHardwareCreateProcessTap failed ({status}); needs macOS 14.2+ and the System Audio Recording permission");
        }

        // The aggregate carries the tap as its input and the real output device
        // as its output, so it shows up wherever an output device is expected.
        let drift_compensation = CFNumber::new_i32(1);
        let tap_entry: CFRetained<CFDictionary<CFString, CFType>> = CFDictionary::from_slices(
            &[&*key(kAudioSubTapUIDKey), &*key(SUB_TAP_DRIFT_COMPENSATION_KEY)],
            &[&*CFString::from_str(&tap_uid) as &CFType, &*drift_compensation as &CFType],
        );
        let sub_device_entry: CFRetained<CFDictionary<CFString, CFType>> =
            CFDictionary::from_slices(&[&*key(kAudioSubDeviceUIDKey)], &[&*output_uid as &CFType]);
        let tap_list: CFRetained<CFArray<CFType>> = CFArray::from_objects(&[&*tap_entry as &CFType]);
        let sub_device_list: CFRetained<CFArray<CFType>> = CFArray::from_objects(&[&*sub_device_entry as &CFType]);
        let one = CFNumber::new_i32(1);
        let name = CFString::from_str(DEVICE_NAME);
        let uid = CFString::from_str(DEVICE_UID);

        let keys = [
            key(kAudioAggregateDeviceUIDKey),
            key(kAudioAggregateDeviceNameKey),
            key(kAudioAggregateDeviceIsPrivateKey),
            key(kAudioAggregateDeviceTapAutoStartKey),
            key(kAudioAggregateDeviceMainSubDeviceKey),
            key(kAudioAggregateDeviceSubDeviceListKey),
            key(kAudioAggregateDeviceTapListKey),
        ];
        let key_refs: Vec<&CFString> = keys.iter().map(|k| &**k).collect();
        let values: [&CFType; 7] = [&uid, &name, &one, &one, &output_uid, &sub_device_list, &tap_list];
        let aggregate_description: CFRetained<CFDictionary<CFString, CFType>> =
            CFDictionary::from_slices(&key_refs, &values);

        let mut aggregate_id: AudioObjectID = 0;
        let status = unsafe {
            AudioHardwareCreateAggregateDevice(aggregate_description.as_opaque(), NonNull::from(&mut aggregate_id))
        };
        if status != 0 || aggregate_id == 0 {
            unsafe { AudioHardwareDestroyProcessTap(tap_id) };
            bail!("AudioHardwareCreateAggregateDevice failed ({status})");
        }

        alvr_common::info!("game audio: tapping system output into \"{DEVICE_NAME}\"");
        Ok(Self { tap_id, aggregate_id })
    }
}

impl Drop for GameAudioCapture {
    fn drop(&mut self) {
        unsafe {
            AudioHardwareDestroyAggregateDevice(self.aggregate_id);
            AudioHardwareDestroyProcessTap(self.tap_id);
        }
    }
}

/// Opens the capture device the way ALVR does (an output device found by
/// name, recorded through its input format) and logs the signal level for
/// `seconds`. Used by WHEELIO_AUDIO_TEST to prove the tap works.
pub fn self_test(seconds: u64) -> Result<()> {
    use cpal::traits::{DeviceTrait, HostTrait, StreamTrait};
    use std::sync::{Arc, Mutex};

    let host = cpal::default_host();
    let device = host
        .output_devices()?
        .find(|d| d.name().map(|n| n.contains(DEVICE_NAME)).unwrap_or(false));
    let Some(device) = device else {
        bail!("\"{DEVICE_NAME}\" is not listed among the output devices");
    };
    let config = device.default_input_config()?;
    alvr_common::info!(
        "audio self-test: {} channels at {} Hz, {:?}",
        config.channels(),
        config.sample_rate().0,
        config.sample_format()
    );

    let peak = Arc::new(Mutex::new((0.0f32, 0usize)));
    let stream = device.build_input_stream(
        &config.into(),
        {
            let peak = Arc::clone(&peak);
            move |data: &[f32], _| {
                let mut lock = peak.lock().unwrap();
                for sample in data {
                    lock.0 = lock.0.max(sample.abs());
                }
                lock.1 += data.len();
            }
        },
        |e| alvr_common::error!("audio self-test stream error: {e}"),
        None,
    )?;
    stream.play()?;

    for second in 1..=seconds {
        std::thread::sleep(std::time::Duration::from_secs(1));
        let mut lock = peak.lock().unwrap();
        alvr_common::info!("audio self-test {second}s: peak {:.3}, {} samples", lock.0, lock.1);
        *lock = (0.0, 0);
    }

    // Teardown check: ALVR captures its network sender inside this callback,
    // so if dropping the stream does not free the callback, the sender (and
    // its UDP socket) leaks and the next connection cannot bind the port.
    drop(stream);
    std::thread::sleep(std::time::Duration::from_secs(1));
    alvr_common::info!(
        "audio self-test: after dropping the stream the callback state has {} owner(s) (1 = freed)",
        Arc::strong_count(&peak)
    );
    let samples_before = peak.lock().unwrap().1;
    std::thread::sleep(std::time::Duration::from_secs(1));
    alvr_common::info!(
        "audio self-test: callback still delivering after drop: {}",
        peak.lock().unwrap().1 != samples_before
    );
    Ok(())
}
