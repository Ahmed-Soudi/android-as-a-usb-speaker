#!/system/bin/sh
# Exclusive USB profile switcher for Samsung/Android configfs gadgets.
# AUDIO mode intentionally disables Android's normal USB functions and exposes
# only UAC1. NORMAL mode hands USB control back to Android/vendor init.

MODDIR=${0%/*}
LOG="$MODDIR/android-usb-speaker.log"
G=/config/usb_gadget/g1
TAG=uac1.android_as_a_usb_speaker
STATE="$MODDIR/.usb_speaker_mode"
SAVED="$MODDIR/.saved_usb_config"
PIDFILE="$MODDIR/.bridge_pid"
IDENTITY="$MODDIR/.saved_usb_identity"
BIN="$MODDIR/bin/android-usb-speaker"
POWER_SAVED="$MODDIR/.saved_udc_power"
WAKELOCK_NAME="android_as_usb_speaker"

log() { echo "$(date '+%F %T') [usb] $*" >> "$LOG"; }

find_cfg() {
  [ -d "$G/configs/b.1" ] && { echo "$G/configs/b.1"; return; }
  [ -d "$G/configs/c.1" ] && { echo "$G/configs/c.1"; return; }
  for d in "$G"/configs/*; do [ -d "$d" ] && { echo "$d"; return; }; done
}

controller() {
  u="$(cat "$G/UDC" 2>/dev/null)"
  [ -n "$u" ] && { echo "$u"; return; }
  u="$(getprop sys.usb.controller 2>/dev/null)"
  [ -n "$u" ] && { echo "$u"; return; }
  for p in /sys/class/udc/*; do [ -e "$p" ] && { basename "$p"; return; }; done
}

unbind_once() {
  [ -d "$G" ] || return 1
  u="$(cat "$G/UDC" 2>/dev/null)"
  [ -z "$u" ] && return 0
  echo "" > "$G/UDC" 2>/dev/null || return 1
  i=0
  while [ "$i" -lt 20 ]; do
    [ -z "$(cat "$G/UDC" 2>/dev/null)" ] && return 0
    sleep 0.1
    i=$((i + 1))
  done
  return 1
}

remove_config_links() {
  for cfg in "$G"/configs/*; do
    [ -d "$cfg" ] || continue
    for l in "$cfg"/*; do [ -L "$l" ] && rm -f "$l" 2>/dev/null; done
  done
}

configure_uac() {
  f="$1"
  [ -d "$f" ] || return 1
  # On this Samsung kernel, the gadget-side ALSA PCM only exposes a capture
  # substream when BOTH UAC directions are enabled. Our proven manual setup
  # produced "playback 1 : capture 1" and arecord then received host audio.
  # Keep both directions at the same 48 kHz / stereo / S16 format.
  [ -e "$f/p_chmask" ] && echo 3 > "$f/p_chmask" || return 1
  [ -e "$f/p_srate" ] && echo 48000 > "$f/p_srate" || return 1
  [ -e "$f/p_ssize" ] && echo 2 > "$f/p_ssize" || return 1
  [ -e "$f/c_chmask" ] && echo 3 > "$f/c_chmask" || return 1
  [ -e "$f/c_srate" ] && echo 48000 > "$f/c_srate" || true
  [ -e "$f/c_ssize" ] && echo 2 > "$f/c_ssize" || true
  [ -e "$f/req_number" ] && echo 2 > "$f/req_number" 2>/dev/null || true
  [ -e "$f/p_mute_present" ] && echo 0 > "$f/p_mute_present" 2>/dev/null || true
  [ -e "$f/p_volume_present" ] && echo 0 > "$f/p_volume_present" 2>/dev/null || true
  [ -e "$f/c_mute_present" ] && echo 0 > "$f/c_mute_present" 2>/dev/null || true
  [ -e "$f/c_volume_present" ] && echo 0 > "$f/c_volume_present" 2>/dev/null || true
}

acquire_audio_power() {
  ctl="$1"
  # Keep the SoC and USB device controller awake while AUDIO ONLY is active.
  # On Samsung, screen-off can otherwise autosuspend the gadget path even while
  # userspace is still alive, leaving the UAC PCM published but no longer
  # delivering frames. All writes are best-effort and are restored on OFF.
  p="/sys/class/udc/$ctl/device/power/control"
  w="/sys/class/udc/$ctl/device/power/wakeup"
  {
    [ -r "$p" ] && cat "$p" || echo ""
    [ -r "$w" ] && cat "$w" || echo ""
  } > "$POWER_SAVED"
  [ -w "$p" ] && echo on > "$p" 2>/dev/null || true
  [ -w "$w" ] && echo enabled > "$w" 2>/dev/null || true
  if [ -w /sys/power/wake_lock ]; then
    echo "$WAKELOCK_NAME" > /sys/power/wake_lock 2>/dev/null || true
  fi
  log "audio power hold: udc_power=$(cat "$p" 2>/dev/null) wakeup=$(cat "$w" 2>/dev/null) wakelock=$WAKELOCK_NAME"
}

release_audio_power() {
  ctl="$1"
  p="/sys/class/udc/$ctl/device/power/control"
  w="/sys/class/udc/$ctl/device/power/wakeup"
  oldp="$(sed -n '1p' "$POWER_SAVED" 2>/dev/null)"
  oldw="$(sed -n '2p' "$POWER_SAVED" 2>/dev/null)"
  [ -n "$oldp" ] && [ -w "$p" ] && echo "$oldp" > "$p" 2>/dev/null || true
  [ -n "$oldw" ] && [ -w "$w" ] && echo "$oldw" > "$w" 2>/dev/null || true
  if [ -w /sys/power/wake_unlock ]; then
    echo "$WAKELOCK_NAME" > /sys/power/wake_unlock 2>/dev/null || true
  fi
  rm -f "$POWER_SAVED"
}

stop_bridge() {
  if [ -f "$PIDFILE" ]; then
    pid="$(cat "$PIDFILE" 2>/dev/null)"
    [ -n "$pid" ] && kill "$pid" 2>/dev/null
    rm -f "$PIDFILE"
  fi
}

start_bridge() {
  stop_bridge
  ALSA_CONFIG_PATH="$MODDIR/alsa.conf" LD_LIBRARY_PATH="$MODDIR/lib64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" "$BIN" --auto >> "$LOG" 2>&1 &
  echo $! > "$PIDFILE"
  log "bridge started pid=$(cat "$PIDFILE")"
}


save_normal_identity() {
  # Preserve the Samsung gadget identity so OFF can restore it before handing
  # control back to Android/vendor init. Empty fields are kept as empty lines.
  {
    cat "$G/idVendor" 2>/dev/null
    cat "$G/idProduct" 2>/dev/null
    cat "$G/strings/0x409/product" 2>/dev/null
    cfg="$(find_cfg)"
    [ -n "$cfg" ] && cat "$cfg/strings/0x409/configuration" 2>/dev/null
  } > "$IDENTITY"
}

set_audio_identity() {
  cfg="$1"
  # Reusing Samsung's normal 04e8:6860 MTP/ADB identity with a UAC-only
  # descriptor caused the host to enumerate audio but never stream into the
  # gadget. The proven manual test succeeds with the Linux USB-audio gadget
  # identity and a matching product/configuration string.
  echo 0x1d6b > "$G/idVendor" || return 1
  echo 0x0101 > "$G/idProduct" || return 1
  [ -e "$G/strings/0x409/product" ] && echo "Android USB Speaker" > "$G/strings/0x409/product"
  [ -n "$cfg" ] && [ -e "$cfg/strings/0x409/configuration" ] && echo "USB Audio" > "$cfg/strings/0x409/configuration"
}

restore_normal_identity() {
  [ -f "$IDENTITY" ] || return 0
  vid="$(sed -n '1p' "$IDENTITY")"
  pid="$(sed -n '2p' "$IDENTITY")"
  product="$(sed -n '3p' "$IDENTITY")"
  config="$(sed -n '4p' "$IDENTITY")"
  [ -n "$vid" ] && echo "$vid" > "$G/idVendor" 2>/dev/null || true
  [ -n "$pid" ] && echo "$pid" > "$G/idProduct" 2>/dev/null || true
  [ -n "$product" ] && [ -e "$G/strings/0x409/product" ] && echo "$product" > "$G/strings/0x409/product" 2>/dev/null || true
  cfg="$(find_cfg)"
  [ -n "$config" ] && [ -n "$cfg" ] && [ -e "$cfg/strings/0x409/configuration" ] && echo "$config" > "$cfg/strings/0x409/configuration" 2>/dev/null || true
}

save_normal_profile() {
  cur="$(getprop sys.usb.config 2>/dev/null)"
  case "$cur" in
    ""|none) cur="$(getprop persist.sys.usb.config 2>/dev/null)" ;;
  esac
  case "$cur" in
    ""|none) cur="mtp,adb" ;;
  esac
  echo "$cur" > "$SAVED"
  log "saved Android USB profile: $cur"
}

audio_on() {
  [ -d "$G" ] || { echo "! configfs gadget g1 not found"; return 1; }
  ctl="$(controller)"
  [ -n "$ctl" ] || { echo "! no UDC controller found"; return 1; }
  cfg="$(find_cfg)"
  [ -n "$cfg" ] || { echo "! no gadget configuration found"; return 1; }

  save_normal_profile
  save_normal_identity
  stop_bridge

  # Ask Android/vendor USB stack to release the gadget first. This is the key
  # difference from v0.2: we do NOT fight MTP/ADB with a repeating rebind loop.
  setprop sys.usb.config none
  stop adbd 2>/dev/null || true
  sleep 0.5
  if ! unbind_once; then
    echo "! could not unbind UDC $ctl"
    log "audio_on failed: unable to unbind $ctl"
    return 1
  fi

  remove_config_links

  set_audio_identity "$cfg" || {
    echo "! could not set dedicated USB-audio identity"
    log "audio_on failed: cannot set 1d6b:0101 audio identity"
    return 1
  }

  # Recreate our UAC1 function cleanly if possible.
  if [ -d "$G/functions/$TAG" ]; then
    rmdir "$G/functions/$TAG" 2>/dev/null || true
  fi
  if [ ! -d "$G/functions/$TAG" ]; then
    mkdir "$G/functions/$TAG" 2>/dev/null || {
      echo "! kernel could not create UAC1 function"
      log "audio_on failed: cannot mkdir $TAG"
      return 1
    }
  fi
  configure_uac "$G/functions/$TAG" || {
    echo "! UAC1 function exists but could not be configured"
    log "audio_on failed: UAC1 attributes unavailable"
    return 1
  }

  ln -s "$G/functions/$TAG" "$cfg/f1" 2>/dev/null || {
    echo "! could not link UAC1 into $(basename "$cfg")"
    log "audio_on failed: cannot link UAC1"
    return 1
  }

  echo "$ctl" > "$G/UDC" 2>/dev/null || {
    rm -f "$cfg/f1"
    echo "! UDC refused UAC1 bind"
    log "audio_on failed: bind $ctl"
    return 1
  }

  echo audio > "$STATE"
  log "AUDIO ONLY enabled on $ctl (1d6b:0101 Android USB Speaker; UAC1 48k stereo S16, dual-direction for gadget capture); Android USB functions disabled"

  acquire_audio_power "$ctl"

  # Wait for the UAC PCM to expose a gadget-side CAPTURE substream.
  # The native bridge records from that substream. Do not start a useless
  # retry loop if the kernel published playback-only.
  i=0
  capture_ready=0
  while [ "$i" -lt 40 ]; do
    if grep -i 'UAC1_PCM' /proc/asound/pcm 2>/dev/null | grep -q 'capture [1-9]'; then
      capture_ready=1
      break
    fi
    sleep 0.1
    i=$((i + 1))
  done
  if [ "$capture_ready" != 1 ]; then
    echo "! UAC1 bound, but gadget capture PCM is missing"
    log "audio_on failed: UAC1 PCM has no capture substream: $(grep -i 'UAC1_PCM' /proc/asound/pcm 2>/dev/null | tr '\n' ' ')"
    release_audio_power "$ctl"
    return 1
  fi
  start_bridge
  return 0
}

audio_off() {
  stop_bridge
  ctl="$(controller)"
  [ -n "$ctl" ] || ctl="$(getprop sys.usb.controller 2>/dev/null)"
  [ -n "$ctl" ] && release_audio_power "$ctl"

  unbind_once 2>/dev/null || true
  remove_config_links
  [ -d "$G/functions/$TAG" ] && rmdir "$G/functions/$TAG" 2>/dev/null || true
  restore_normal_identity

  restore="$(cat "$SAVED" 2>/dev/null)"
  case "$restore" in ""|none) restore="mtp,adb";; esac

  # Hand the gadget back to Samsung/Android. Their init/HAL recreates the proper
  # MTP/ACM/ADB FunctionFS links and descriptors for this device.
  setprop sys.usb.config none
  sleep 0.25
  setprop sys.usb.config "$restore"
  case ",$restore," in *,adb,*) start adbd 2>/dev/null || true;; esac

  rm -f "$STATE"
  log "AUDIO ONLY disabled; restored Android USB profile: $restore"
  return 0
}

status() {
  mode=normal
  [ "$(cat "$STATE" 2>/dev/null)" = audio ] && mode=audio
  echo "Mode: $mode"
  echo "USB identity: $(cat "$G/idVendor" 2>/dev/null):$(cat "$G/idProduct" 2>/dev/null)  product=$(cat "$G/strings/0x409/product" 2>/dev/null)"
  echo "sys.usb.config: $(getprop sys.usb.config 2>/dev/null)"
  echo "sys.usb.state:  $(getprop sys.usb.state 2>/dev/null)"
  echo "UDC: $(cat "$G/UDC" 2>/dev/null)"
  u="$(cat "$G/UDC" 2>/dev/null)"
  [ -n "$u" ] && echo "UDC state: $(cat "/sys/class/udc/$u/state" 2>/dev/null)"
  echo "Functions:"
  for l in "$G"/configs/*/*; do
    [ -L "$l" ] && echo "  $(basename "$l") -> $(basename "$(readlink "$l")")"
  done
  echo "UAC card:"
  grep -i -A1 'UAC' /proc/asound/cards 2>/dev/null || true
  echo "UAC PCM:"
  grep -i 'UAC1_PCM' /proc/asound/pcm 2>/dev/null || true
  echo "ADB: $(getprop init.svc.adbd 2>/dev/null)"
}

case "$1" in
  on|audio_on) audio_on ;;
  off|audio_off) audio_off ;;
  status) status ;;
  *) echo "usage: $0 {on|off|status}"; exit 2 ;;
esac
