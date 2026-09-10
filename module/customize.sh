#!/system/bin/sh
ui_print "- Android as a USB Speaker v0.11.9"
ui_print "- Checking device..."
ABI="$(getprop ro.product.cpu.abi)"
API="$(getprop ro.build.version.sdk)"
[ "$ABI" = "arm64-v8a" ] || abort "! arm64-v8a required (found: $ABI)"
[ "$API" -ge 28 ] || abort "! Android 9 / API 28+ required (found: $API)"
set_perm "$MODPATH/service.sh" 0 0 0755
set_perm "$MODPATH/usb-gadget.sh" 0 0 0755
set_perm "$MODPATH/action.sh" 0 0 0755
set_perm "$MODPATH/bin/android-usb-speaker" 0 0 0755
ui_print "- Safe default: NORMAL Android USB / ADB"
ui_print "- Module Action toggles: NORMAL USB <-> AUDIO-ONLY"
ui_print "- No background UDC rebind loop"
ui_print "- No Termux / FFmpeg / VLC runtime dependency"
ui_print "- No Zygisk dependency"
ui_print "- No SELinux policy installed"
ui_print "- Reboot after installation"
