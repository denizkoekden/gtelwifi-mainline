#!/bin/sh
# DSI host probing from user space (D2a prototype, vendor-ref/research/dsi-report.md
# §6.1/§6.2). Runs on the tablet; needs a devmem (BB=/var/tmp/busybox by default).
#
#   dsi-probe.sh audit            gates and DSI registers (no writes; INT_ST0/1 not read)
#   dsi-probe.sh read <reg>       DCS read in LP command mode, e.g. 0x80 (ID, expect 0x03),
#                                 0x0a (power mode, expect 0x9a when on)
#   dsi-probe.sh write <b0> [b1]  DCS short write in LP command mode (0x10 sleep in, 0x11 out)
#   dsi-probe.sh dispc stop|run   stop DISPC without reset / run it again (first-frame path)
#
# Every command that writes switches the host to LP command mode and back to
# video mode (vendor set_lp_mode / dsi_ready order).
BB=${BB:-/var/tmp/busybox}
D=0x21800000
DC=0x20800000

reg() { printf '0x%08x' $(( $1 + $2 )); }
rd() { $BB devmem "$(reg $D "$1")" 32; }
wr() { $BB devmem "$(reg $D "$1")" 32 "$2"; }
rdc() { $BB devmem "$(reg $DC "$1")" 32; }
wrc() { $BB devmem "$(reg $DC "$1")" 32 "$2"; }

gates() {
	eb=$($BB devmem 0x20d00000 32)
	echo "AP_AHB_EB $eb RST $($BB devmem 0x20d00004 32) MISC_CKG_EN $($BB devmem 0x20d03008 32)" \
	     "PWR_CTRL $($BB devmem 0x402e0024 32)"
	[ $(( eb & 1 )) -eq 1 ] || { echo "DSI_EB is 0, not touching the DSI host"; exit 1; }
}

enter_cmd() {
	s_mode=$(rd 0x34); s_cmd=$(rd 0x68); s_lpclk=$(rd 0x94); s_pck=$(rd 0x2c)
	wr 0x34 1
	wr 0x68 0x010F7F00
	wr 0x94 0
}

leave_cmd() {
	wr 0x2c "$s_pck"
	wr 0x68 "$s_cmd"
	wr 0x94 1
	wr 0x34 0
	echo "back in video mode: MODE_CFG $(rd 0x34) LPCLK $(rd 0x94) PHY_STATUS $(rd 0xb0)" \
	     "INT_ST0 $(rd 0xbc) INT_ST1 $(rd 0xc0)"
}

# wait until (CMD_PKT_STATUS & mask) == want, 100 tries
wait_st() {
	i=0
	while [ $i -lt 100 ]; do
		st=$(rd 0x74)
		[ $(( st & $1 )) -eq $(( $2 )) ] && return 0
		i=$((i + 1))
	done
	echo "timeout: CMD_PKT_STATUS $st (mask $1 want $2)"
	return 1
}

dcs_read() {
	wr 0x2c 0x0c		# BTA + ECC rx, no EoTp (as sboot's read)
	wait_st 0x2 0 || return 1
	rd 0xbc >/dev/null; rd 0xc0 >/dev/null
	wr 0x6c "$(printf '0x%08x' $(( ($1 << 8) | 0x06 )))"
	wait_st 0x50 0 || { echo "INT_ST1 $(rd 0xc0)"; return 1; }
	v=$(rd 0x70)
	printf 'DCS read 0x%02x = 0x%02x (GEN_PLD_DATA %s, INT_ST1 %s)\n' "$1" $(( v & 0xff )) "$v" "$(rd 0xc0)"
}

dcs_write() {
	wait_st 0x2 0 || return 1
	rd 0xbc >/dev/null; rd 0xc0 >/dev/null
	wr 0x6c "$(printf '0x%08x' $(( (${2:-0} << 16) | ($1 << 8) | 0x15 )))"
	wait_st 0x5 0x5 || return 1
	printf 'DCS write 0x%02x 0x%02x done, PHY_STATUS %s INT_ST1 %s\n' "$1" "${2:-0}" "$(rd 0xb0)" "$(rd 0xc0)"
}

case $1 in
audit)
	gates
	for o in 0x00 0x04 0x08 0x10 0x14 0x18 0x2c 0x34 0x38 0x3c 0x48 0x4c 0x50 0x54 0x58 0x5c 0x60 \
		 0x68 0x74 0x78 0x94 0x98 0x9c 0xa0 0xa4 0xa8 0xb0 0xc4 0xc8; do
		echo "DSI $o = $(rd $o)"
	done
	echo "DISPC CTRL $(rdc 0x0) DPI_CTRL $(rdc 0x80) DPI_STS1 $(rdc 0x90) INT_EN $(rdc 0x70)"
	;;
read)
	gates >/dev/null
	enter_cmd
	dcs_read "$(( $2 ))"
	leave_cmd
	;;
write)
	gates >/dev/null
	enter_cmd
	wr 0x2c 0x0c
	dcs_write "$(( $2 ))" "$(( ${3:-0} ))"
	leave_cmd
	;;
dispc)
	case $2 in
	stop)	# vendor dispc_stop_for_feature: DPI_CTRL bit 4, CTRL bit 4 off, busy (DPI_STS1 bit 16) clear
		wrc 0x80 $(( $(rdc 0x80) | 0x10 ))
		wrc 0x0 $(( $(rdc 0x0) & ~0x10 ))
		i=0; while [ $(( $(rdc 0x90) & 0x10000 )) -ne 0 ] && [ $i -lt 100 ]; do i=$((i + 1)); done
		echo "DISPC stopped: CTRL $(rdc 0x0) DPI_CTRL $(rdc 0x80) DPI_STS1 $(rdc 0x90)" ;;
	run)	# first-frame path: DPI_CTRL bit 5, 30 us, clear bit 4, CTRL bit 4 on
		wrc 0x80 $(( $(rdc 0x80) | 0x20 ))
		wrc 0x80 $(( $(rdc 0x80) & ~0x10 ))
		wrc 0x0 $(( $(rdc 0x0) | 0x10 ))
		echo "DISPC running: CTRL $(rdc 0x0) DPI_CTRL $(rdc 0x80) DPI_STS1 $(rdc 0x90)" ;;
	esac
	;;
*)
	sed -n '2,13p' "$0"; exit 2 ;;
esac
