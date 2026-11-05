#!/bin/sh
# Kameras über PipeWire testen, ohne dass jemand eingeloggt sein muss (Fehler 4,
# „target not found“). Läuft als root auf dem Tab:
#   tools/nurassh 'cat > /tmp/cam-pw-test.sh' < tools/lab/cam-pw-test.sh
#   tools/nurassh 'sh /tmp/cam-pw-test.sh; rm /tmp/cam-pw-test.sh'
# Startet die Benutzerdienste von user (10000), öffnet die Kamerageräte kurz für
# alle (die uaccess-ACLs hat nur die aktive Sitzung), startet PipeWire und
# WirePlumber neu und holt von jeder Kamera 15 Bilder per pipewiresrc. Danach
# Rechte und Dienste wie vorher. Nicht laufen lassen, während eine Kamera streamt.
# Ohne den client-node-Fix (pipewire < r90): beide „target not found“.
W="runuser -u user -- env XDG_RUNTIME_DIR=/run/user/10000 DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/10000/bus"
D="/dev/video0 /dev/media0 /dev/v4l-subdev0 /dev/v4l-subdev1"
was=$(systemctl is-active user@10000)
systemctl start user@10000
chmod 666 $D
$W systemctl --user restart pipewire pipewire-pulse wireplumber
sleep 8
$W wpctl status 2>/dev/null | sed -n '/^Video/,/^Settings/p' | grep -E "Kamera|camera|libcamera"
for cam in 56 20; do
	echo "--- Kamera $cam (I2C-Adresse 0x$cam)"
	# ohne media.type=Video ist der Stream „Stream/Input/Unknown“ und findet nie ein Ziel
	$W timeout 40 gst-launch-1.0 pipewiresrc target-object=libcamera_input._base_soc_i2c_70500000_camera_$cam \
		stream-properties="props,media.type=Video,media.category=Capture,media.role=Camera" num-buffers=15 \
		! fakesink sync=false 2>&1 | grep -E "EOS|Execution ended|ERROR|target" | head -3
	sleep 2
done
$W journalctl --user -u wireplumber --since "-60s" --no-pager -o cat 2>/dev/null | grep -iE "abort|PropInfo" | head -5
chmod 660 $D
[ "$was" = active ] || systemctl stop user@10000
ls -l $D | awk '{print $1, $NF}'
