#!/bin/sh
# Speaker path power steps one by one, 5 s apart, step number on the console.
A=/sys/kernel/debug/sprd-adi
rd() { echo $1 > $A/address; cat $A/value; }
wr() { echo $1 > $A/address; echo $2 > $A/value; }
st() { v=$(rd $1); wr $1 $(printf 0x%x $(( (v & ~$2) | $3 ))); }
step() { msg="SCHRITT $1: $2"; echo "$msg" > /dev/kmsg; printf "\n\n   >>> %s <<<\n" "$msg" > /dev/tty0; echo "$(date +%T) $msg"; sleep 5; }
/tmp/pmic-wdg.sh arm 120
sleep 15
st 0x61c 0x900 0x900;		step 1 "DA- und DRV-Takt an"
st 0x624 0xc000 0xc000; st 0x68c 0xa00 0xa00;	step 2 "DACL/DACR an"
st 0x624 0x10 0x10;		step 3 "AOL an"
st 0x600 0x8000 0x8000;		step 4 "PA-Versorgung (VBAT) an"
st 0x60c 0x3800 0; st 0x614 0xe000 0x6000; st 0x62c 0x8000 0; wr 0x618 0x400
st 0x624 0x2c00 0x2c00; st 0x62c 0x1000 0x1000; st 0x618 0xc000 0; st 0x600 0x4000 0
st 0x62c 0x6000 0x6000;		step 5 "PA konfiguriert (DEMI, DTRI 3)"
st 0x600 0x2000 0x2000;		step 6 "PA an"
st 0x600 0x2000 0;		step 7 "PA aus"
st 0x600 0x8000 0;		step 8 "PA-Versorgung aus"
st 0x624 0x10 0;		step 9 "AOL aus"
st 0x624 0xc000 0; st 0x68c 0xa00 0;	step 10 "DACL/DACR aus"
st 0x61c 0x900 0;		step 11 "Takte aus"
/tmp/pmic-wdg.sh disarm
