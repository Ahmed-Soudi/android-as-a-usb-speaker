#!/system/bin/sh
MODDIR=${0%/*}
LOG="$MODDIR/android-usb-speaker.log"

# v0.5 is deliberately passive at boot. Samsung/Android owns USB normally.
# The module Action button explicitly toggles AUDIO-ONLY <-> NORMAL USB.
until [ "$(getprop sys.boot_completed)" = "1" ]; do sleep 2; done
rm -f "$MODDIR/.usb_speaker_mode" "$MODDIR/.bridge_pid"
echo "$(date '+%F %T') service ready; NORMAL USB untouched; use module Action to enable audio" >> "$LOG"
