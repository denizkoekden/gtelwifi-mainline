#!/bin/sh
# Manual GPU_TOP power-up as in mach-sprd/pm-domains.c, read the Mali
# version registers, power down again. PMIC watchdog as safety net.
b=/var/tmp/busybox
hex() { printf '0x%08x' "$1"; }
/var/tmp/pmic-wdg.sh arm 120
cfg=$($b devmem 0x402b0020 32)
$b devmem 0x402b0020 32 $(hex $(( (cfg & ~0x03ffffff) | 0x010101 )))
i=0
while :; do
	st=$($b devmem 0x402b00bc 32)
	[ $(( (st >> 24) & 0xf )) -eq 0 ] && break
	i=$((i + 1)); [ $i -ge 200 ] && break
done
echo "PD_GPU_TOP_CFG $cfg -> $($b devmem 0x402b0020 32), PWR_STATUS0 $st after $i polls"
if [ $(( (st >> 24) & 0xf )) -eq 0 ]; then
	eb=$($b devmem 0x402e0000 32)
	$b devmem 0x402e0000 32 $(hex $(( eb | 0x08000000 )))
	echo "EB0 $eb -> $($b devmem 0x402e0000 32)"
	echo "GPU_APB CLK_CTRL $($b devmem 0x60100004 32)"
	$b devmem 0x60100004 32 ${GPU_SEL:-2}
	echo "GPU_APB CLK_CTRL now $($b devmem 0x60100004 32)"
	echo "GP_VERSION  $($b devmem 0x6000006c 32)"
	echo "L2 0x00 $($b devmem 0x60001000 32)  0x04 $($b devmem 0x60001004 32)"
	echo "PP0_VERSION $($b devmem 0x60009000 32)  PP1_VERSION $($b devmem 0x6000b000 32)"
	echo "GPMMU STATUS $($b devmem 0x60003004 32)  PMU 0x08 $($b devmem 0x60002008 32)"
	eb=$($b devmem 0x402e0000 32)
	$b devmem 0x402e0000 32 $(hex $(( eb & ~0x08000000 )))
fi
cfg=$($b devmem 0x402b0020 32)
$b devmem 0x402b0020 32 $(hex $(( cfg | 0x02000000 )))
sleep 0.2
echo "off again: PD $($b devmem 0x402b0020 32) status $($b devmem 0x402b00bc 32) EB0 $($b devmem 0x402e0000 32)"
/var/tmp/pmic-wdg.sh disarm
