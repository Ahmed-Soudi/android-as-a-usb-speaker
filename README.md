# v0.11.9

O32 identified a second ALSA XRUN path: `snd_pcm_wait()` can return `-EPIPE` / Broken pipe. v0.11.8 treated that wait-side XRUN as fatal and restarted the whole bridge, producing the audible cut-then-work cycle. v0.11.9 runs `snd_pcm_recover()` on wait-side errors in place, matching the existing recovery used for `snd_pcm_readi()`. The timeout/ALSA-reopen/UDC-rebind escalation remains available for genuine UAC stalls.

# v0.11.8 persistent UAC stall recovery

O31 showed that ALSA could reopen `hw:3,0` indefinitely while captured frames stayed frozen. v0.11.8 reopens ALSA once, then after ~3 seconds of continued no-data on a previously active stream performs a short UDC unbind/rebind. The UAC configuration remains intact; the host simply re-enumerates the USB Audio device and the supervisor rediscovers/restarts the PCM automatically.

# v0.11.8 screen-off / USB autosuspend fix

Adds an AUDIO-ONLY power hold: legacy Android kernel wakelock when available, forces the UDC runtime PM control to `on`, enables UDC wakeup, restores the original values on Action OFF, and retains v0.11.6 ALSA timeout recovery. This targets the O30 failure where capture froze while the playback thread continued.

# v0.11.8 packaging fix

Fixes stale v0.11.3 GitHub Actions artifact/module packaging labels in the v0.11.4 source. Runtime audio code is the v0.11.4 dual-thread + jitter-buffer build.

# v0.11.4 USB suspend fix

O28 proved the native bridge now opens alsa-lib successfully, opens AAudio MEDIA, and reaches `LIVE`. It then tore itself down because the SM-M127F UDC briefly reported `suspended`; v0.11.1 only accepted `configured`. v0.11.4 treats both `configured` and `suspended` as valid bound states, so USB power-management suspend no longer causes the audio bridge to restart.

# v0.11.1 capture architecture

v0.11.1 deliberately removes the hand-written raw ALSA ioctl state machine. On the target SM-M127F, `arecord -D hw:3,0` has already proven continuous 48 kHz / stereo / S16 capture at period 240 / buffer 960. The module now calls alsa-lib (`snd_pcm_open`, `snd_pcm_hw_params`, `snd_pcm_sw_params`, `snd_pcm_readi`, `snd_pcm_wait`, `snd_pcm_recover`) directly and bundles the Android/Termux aarch64 alsa-lib runtime into the module at build time. This preserves the exact mmap-control / `SYNC_PTR` fallback logic that the successful arecord path uses, without requiring Termux on the installed phone.

# Android as a USB Speaker — v0.9 prototype

Universal Magisk / KernelSU Next module source for Samsung SM-M127F / Android 13.

## Why v0.3 changed USB handling

v0.2 tried to preserve Samsung's live MTP/ADB composite gadget and inject UAC1 into it. On the SM-M127F the vendor USB stack repeatedly reclaimed/rebuilt the gadget, causing UDC rebind failures and visible connect/disconnect loops on the laptop.

v0.9 keeps an explicit **exclusive toggle** from the module manager's **Action** button:

- **NORMAL USB** (default): Android/Samsung owns USB normally; MTP/ADB work.
- Tap **Action** -> **AUDIO ONLY**: Android USB functions are released and a UAC1-only gadget is bound once; the low-latency audio bridge starts.
- Tap **Action** again -> **NORMAL USB**: audio bridge stops and the previously saved Android USB profile (normally `mtp,adb`) is restored.
- Reboot always returns to the safe NORMAL USB state.

There is no background gadget-monitor/rebind loop.

## v0.4 capture fix

On the SM-M127F, a UAC1 function configured with only `p_*` produced `/proc/asound/pcm` as `playback 1` only, so TinyALSA `PCM_IN` could not open it. The earlier manually-proven setup exposed `playback 1 : capture 1` and `arecord` successfully received host audio. v0.4 therefore enables both `p_*` and `c_*` at stereo / 48 kHz / S16 and waits for a capture-capable `UAC1_PCM` before starting the bridge.

## v0.5 USB identity fix

The SM-M127F's normal Samsung gadget advertises `04e8:6860`, `SAMSUNG_Android`, and an `mtp_acm_adb` configuration. Reusing that identity for a UAC-only configuration let Windows enumerate an audio endpoint but gadget-side `arecord` failed with `EIO`. A manual rebind using `1d6b:0101`, product `Android USB Speaker`, and configuration `USB Audio` immediately produced a valid 48 kHz stereo WAV. v0.5 applies that dedicated identity only in AUDIO mode and restores the saved Samsung identity before returning control to Android in NORMAL mode.

The native bridge now discovers both ALSA card and device from `/proc/asound/pcm` instead of assuming card 3 / device 0.

## Audio path

`PS5/laptop -> UAC1 gadget -> raw ALSA capture -> zero-lookahead leveler/limiter -> AAudio USAGE_MEDIA -> Samsung Audio HAL -> ABOX/AW8896 speaker`

48 kHz, stereo, S16 end-to-end. Runtime does not depend on Termux, FFmpeg, VLC, Zygisk, or a custom SELinux policy.


## v0.6/v0.7 capture findings

Diagnostics proved the SM-M127F UAC1 endpoint works at 48 kHz, stereo, S16_LE with a 240-frame period and 960-frame buffer. v0.6 proved AAudio MEDIA output also opens successfully, but TinyALSA failed during capture reads. v0.7 therefore switched capture to direct ALSA kernel ioctls.

## v0.9 SYNC_PTR fix

O22 measured the exact working `arecord` software parameters while the UAC stream was actively running:

- `tstamp_mode: NONE`
- `avail_min: 240`
- `start_threshold: 1`
- `stop_threshold: 960`
- `silence_threshold: 0`
- `silence_size: 0`
- `boundary: 8646911284551352320`

v0.7 incorrectly submitted `avail_min=0`, which Linux rejects with `EINVAL` in `snd_pcm_sw_params()`. v0.9 mirrors the proven `arecord` state, then calls `PREPARE` and uses blocking `READI_FRAMES` with XRUN/suspend recovery. USB identity, AAudio MEDIA output, DSP, Action toggle, boot-OFF behavior, and SELinux handling are unchanged.

## Build

Push this repository to GitHub and run **Build universal module** from Actions. It produces:

`android-as-a-usb-speaker-v0.11.1.1.zip`

Install that generated ZIP from Magisk Manager or KernelSU Next Manager and reboot.

## Use

Open the module in the root manager and press **Action**:

1. NORMAL -> AUDIO ONLY: ADB/MTP intentionally disconnect; USB host should enumerate the phone as UAC1 audio.
2. AUDIO ONLY -> NORMAL: UAC/audio stops; Android's saved USB profile is restored.

This is intentional for hosts such as PS5 and for Samsung devices where UAC+ADB composite mode is unstable.

## SELinux

No `sepolicy.rule`, no permissive switch, and no Zygisk dependency. SELinux remains Enforcing. If native audio access is denied, diagnose the exact AVC before considering any policy change.

## v0.9 capture backend

v0.9 uses the raw ALSA runtime capture path introduced in v0.7 and fixes its SW_PARAMS setup. Diagnostics on SM-M127F proved the UAC1 endpoint records successfully with `arecord` at 48 kHz, stereo, S16_LE, 240-frame periods and a 960-frame buffer, while TinyALSA repeatedly failed at the first/early read. The native bridge now opens `/dev/snd/pcmC*D*c` directly in blocking mode and programs ALSA with the kernel UAPI ioctls (`HW_REFINE`, `HW_PARAMS`, `SW_PARAMS`, `PREPARE`, `READI_FRAMES`). It keeps the proven 240/960 geometry and adds XRUN/suspend recovery plus detailed status logging. AAudio MEDIA output, DSP, Action toggle, dedicated UAC1 identity, boot-OFF behavior and SELinux policy remain unchanged.


## v0.9 O25 fix

O25 showed the known-good alsa-lib/arecord path issuing `SNDRV_PCM_IOCTL_SYNC_PTR` after successful `READI_FRAMES` operations. v0.9 mirrors alsa-lib fallback behavior by synchronizing `APPL|AVAIL_MIN` immediately after `PREPARE` and after every successful `READI_FRAMES`. This is intentionally limited to the capture backend; USB gadget setup, AAudio MEDIA output, DSP, Action toggle, boot-OFF behavior, and SELinux behavior are unchanged.


## v0.11.1 build fix
The GitHub Actions workflow now resolves Termux package filenames from the repository Packages index instead of hard-coding versioned .deb pool URLs. This prevents 404 build failures when Termux rotates package versions.


### v0.11.4
- Replaced callback/ring AAudio output with direct blocking `AAudioStream_write()` per 5 ms capture block.
- Added two-second STREAM counters (`captured`, `written`, `peak`, AAudio state/xruns) to make silence diagnosable from logcat.


## v0.11.4
- Decouples USB capture and AAudio playback with a 40 ms jitter buffer.
- Prefills before playback starts.
- Increases AAudio buffer to ~40 ms to survive USB scheduling gaps.
- Adds ring/soft-underflow/drop telemetry to diagnose clock drift without cutting audio.
