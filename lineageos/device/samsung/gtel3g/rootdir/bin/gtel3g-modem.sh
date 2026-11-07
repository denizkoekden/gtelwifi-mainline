#!/vendor/bin/sh
#
# SM-T561 (gtel3g): load and start the modem core (CP), then wait until it
# answers on its AT channel. Same steps as out/gtel3g-modem/cp-test.sh, which
# ran on a T561 (rev10) on 07.11.2026: MODEM and WDSP from the device's own
# partitions into the CP memory window (/proc/cpw, kernel patch 0107), then
# "start". Writes nothing to flash.
#
# Sets vendor.gtel3g.cp = running | failed | absent (init.gtel3g.rc starts
# nvitemd and the radio HAL on "running").

P=/proc/cpw
B=/dev/block/by-name
AT=/dev/stty_w0
TMP=/data/vendor/radio/at-probe.out

log() { /system/bin/log -p i -t gtel3g-modem "$*"; }

if [ "$(getprop persist.vendor.gtel3g.modem)" = "0" ]; then
	log "CP off (persist.vendor.gtel3g.modem=0)"
	setprop vendor.gtel3g.cp off
	exit 0
fi

if [ ! -e $P/start ]; then
	log "no $P: kernel without SPRD_CPROC"
	setprop vendor.gtel3g.cp absent
	exit 0
fi

# [V] whether sboot already preloads MODEM/WDSP is unknown; loading again
# is what the test did. A running CP is stopped first (HAL restart case).
if grep -q started $P/status 2>/dev/null; then
	log "CP was running, stopping it"
	echo 1 > $P/stop
	sleep 1
fi

M=$B/MODEM
D=$B/WDSP
if ! dd if=$M of=$P/modem bs=65536 2>/dev/null; then
	log "loading $M failed"
	setprop vendor.gtel3g.cp failed
	exit 1
fi
if ! dd if=$D of=$P/dsp bs=65536 2>/dev/null; then
	log "loading $D failed"
	setprop vendor.gtel3g.cp failed
	exit 1
fi

echo 1 > $P/start
log "CP started: $(cat $P/status)"

# The SIPC channels came up about 1 s after "start" in the test; the AT
# channel answered OK. Probe for up to ~60 s.
i=0
while [ $i -lt 20 ]; do
	i=$((i + 1))
	sleep 2
	[ -c $AT ] || continue
	rm -f $TMP
	( timeout 3 cat $AT > $TMP 2>/dev/null & )
	sleep 0.3
	timeout 2 sh -c "printf 'AT\r' > $AT" 2>/dev/null
	sleep 1
	if grep -q OK $TMP 2>/dev/null; then
		rm -f $TMP
		log "AT channel answers (try $i)"
		setprop vendor.gtel3g.cp running
		exit 0
	fi
done

rm -f $TMP
log "no answer on $AT, status: $(cat $P/status)"
setprop vendor.gtel3g.cp failed
exit 1
