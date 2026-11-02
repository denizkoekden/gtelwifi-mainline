#!/bin/sh
# Safety net for suspend experiments on the tablet: arm the SC2723 PMIC
# watchdog so that a system that never comes back resets itself after N
# seconds (warm reset like our restart handler, so ramoops survives),
# instead of needing Power + Volume Down. Runs on the tablet, needs the
# sprd-adi debugfs (patch 0030) and devmem.
#
#   pmic-wdg.sh arm <seconds>  |  pmic-wdg.sh disarm  |  pmic-wdg.sh status
#
# Register sequence as in mach-sprd/restart.c: GLB 0x800/0x808 bit 2 =
# watchdog module and clock, LOCK 0x60 = 0xe551 to unlock, CTRL 0x48
# (bit 1 count, bit 2 new mode, bit 3 reset), LOAD 0x40/0x44 in 32 kHz
# ticks, INT_RAW 0x50 bit 4 = load busy.
set -e
A=/sys/kernel/debug/sprd-adi

rd() { echo "$1" > $A/address; v=$(cat $A/value); echo $((v)); }
wr() { echo "$1" > $A/address; echo "$2" > $A/value; }
rmw() { wr "$1" $(( ($(rd "$1") & ~$2) | $3 )); }

case $1 in
arm)
	t=$(( ${2:?seconds} * 32768 ))
	# what our restart handler leaves for sboot: no upload, no
	# power-off charging, normal boot (IRAM keeps the values of the last
	# restart anyway, Nura has no devmem)
	if command -v devmem > /dev/null; then
		devmem 0xf80 32 0
		devmem 0xf8c 32 0x12345678
		devmem 0xf90 32 0x12345670
	fi
	rmw 0x800 0 4
	rmw 0x808 0 4
	wr 0x60 0xe551
	rmw 0x48 0 4
	wr 0x44 $(( t >> 16 ))
	wr 0x40 $(( t & 0xffff ))
	i=0
	while [ $(( $(rd 0x50) & 0x10 )) -ne 0 ] && [ $i -lt 100 ]; do i=$((i + 1)); done
	rmw 0x48 0 10
	wr 0x60 $(( ~0xe551 & 0xffff ))
	echo "PMIC watchdog armed, reset in $2 s"
	;;
disarm)
	wr 0x60 0xe551
	rmw 0x48 10 0
	wr 0x60 $(( ~0xe551 & 0xffff ))
	echo "PMIC watchdog off"
	;;
status)
	printf 'CTRL %#x LOAD %#x%04x\n' $(rd 0x48) $(rd 0x44) $(rd 0x40)
	;;
*)
	echo "usage: $0 arm <seconds> | disarm | status" >&2
	exit 2
	;;
esac
