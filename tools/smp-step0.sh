#!/bin/sh
# SMP step 0 on the tablet (rescue system, UP kernel, busybox devmem):
# read the CPU power/reset registers, power CPU1 on into a probe stub in
# IRAM0 that reports MPIDR/CPSR/NSACR, power it off again; then check
# whether the CA7 timestamp counter TS0 can be enabled.
# See vendor-ref/research/smp-report.md 5.1. Needs /tmp/sev (out/sev).
#   tools/tabssh 'cat > /tmp/sev; chmod +x /tmp/sev' < out/sev
#   tools/tabssh 'sh -s' < tools/smp-step0.sh
r() { printf '%-11s %s\n' "$1" "$(devmem "$1" 32)"; }
hex() { printf '0x%08X' "$1"; }

echo "== registers before"
for a in 0x402B00BC 0x402B0004 0x402B0008 0x402B000C 0x402B0010 \
	 0x402B00E8 0x402B00EC 0x402B00F0 0x402B00F4 \
	 0x20D00008 0x20D00010 0x20D00014 0x20D0001C 0x20D03004 0x20D03034 \
	 0x402E0000 0x402E0024 0x402E0028 0x402D004C; do
	r $a
done

echo "== CPU1 into the stub at 0x800"
i=0
for w in e3a00d21 ee101fb0 e5801000 e10f2000 e5802004 ee113f51 \
	 e5803008 f57ff04f e320f003 eafffffd; do
	devmem $((0x800 + i)) 32 0x$w
	i=$((i + 4))
done
devmem 0x840 32 0; devmem 0x844 32 0; devmem 0x848 32 0
devmem 0x20D0001C 32 0x800
devmem 0x20D00014 32 $(hex $(( $(devmem 0x20D00014) | 0x2 )))
devmem 0x20D00008 32 $(hex $(( $(devmem 0x20D00008) | 0x2 )))
devmem 0x402B0008 32 0x00040404
sleep 0.1
r 0x402B00BC
devmem 0x20D00008 32 $(hex $(( $(devmem 0x20D00008) & ~0x2 )))
/tmp/sev
sleep 0.2
echo "== mailbox (expect MPIDR 0x80000F01, CPSR mode, NSACR)"
r 0x840; r 0x844; r 0x848; r 0x20D03034
if [ "$(devmem 0x840 32)" = 0x00000000 ]; then
	echo "no answer yet, SEV again"
	/tmp/sev; sleep 0.2
	r 0x840; r 0x844; r 0x848; r 0x20D03034
fi

echo "== CPU1 off again"
r 0x20D03034
devmem 0x402B0008 32 $(hex $(( $(devmem 0x402B0008) | 0x02000000 )))
sleep 0.1
r 0x402B00BC

echo "== TS0 timestamp counter"
devmem 0x402E0000 32 $(hex $(( $(devmem 0x402E0000) | 0x10000000 )))
devmem 0x402D004C 32 $(hex $(( ($(devmem 0x402D004C) & ~3) | 1 )))
for o in 0x000 0x004 0x008 0x00C 0x020 0xFD0 0xFE0 0xFE4 0xFE8 0xFEC 0xFF0 0xFF4 0xFF8 0xFFC; do
	r $(hex $((0x40400000 + o)))
done
devmem 0x40400000 32 $(hex $(( $(devmem 0x40400000) | 1 )))
a=$(devmem 0x40400008 32); sleep 1; b=$(devmem 0x40400008 32)
echo "TS0 CNTCV low: $a -> $b (delta $(( b - a )) in ~1 s)"
