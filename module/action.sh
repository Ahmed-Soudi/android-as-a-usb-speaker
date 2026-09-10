#!/system/bin/sh
MODDIR=${0%/*}
USB="$MODDIR/usb-gadget.sh"
STATE="$MODDIR/.usb_speaker_mode"
LOG="$MODDIR/android-usb-speaker.log"

echo "=== Android as a USB Speaker ==="
echo "SELinux: $(getenforce 2>/dev/null)"

if [ "$(cat "$STATE" 2>/dev/null)" = audio ]; then
  echo ""
  echo "AUDIO mode is ON -> switching back to NORMAL USB (ADB/MTP)..."
  if "$USB" off; then
    sleep 1
    echo "✓ USB speaker OFF"
    echo "✓ Android USB/ADB restored"
  else
    echo "! Failed to restore normal USB. Reboot will let Android reclaim USB."
  fi
else
  echo ""
  echo "NORMAL USB is active -> switching to AUDIO-ONLY mode..."
  echo "ADB/MTP will disconnect intentionally."
  if "$USB" on; then
    sleep 1
    echo "✓ USB speaker ON"
    echo "✓ UAC1 audio-only gadget active"
    grep -i 'UAC1_PCM' /proc/asound/pcm 2>/dev/null | sed 's/^/  PCM: /' 
  else
    echo "! Failed to enable audio-only mode."
    echo "  Running restore for safety..."
    "$USB" off >/dev/null 2>&1
  fi
fi

echo ""
echo "--- status ---"
"$USB" status 2>/dev/null
echo "--- last log ---"
tail -n 20 "$LOG" 2>/dev/null
