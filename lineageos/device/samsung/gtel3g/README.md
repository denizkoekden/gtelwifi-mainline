# SM-T561 (gtel3g): 3G für LineageOS 24 – Gerätebaum und Radio-HAL

Stand 07.11.2026, **ENTWURF, nicht gebaut, nicht auf einem Gerät gelaufen.** Ziel: mobile Daten und SMS über den
SC7730-Modemkern (CP) des SM-T561, **keine Sprache** (Tablet nicht voice-capable, kein Audiopfad über den CP).
Ohne die alten LOS-16-RIL-Blobs (kein rild, keine libril, kein phoneserver, keine Shims).

[B] = belegt (Datei/Test), [V] = Vermutung, beim ersten Test prüfen.

## Aufbau

```
init (post-fs-data)
 └─ vendor.gtel3g-modem  = rootdir/bin/gtel3g-modem.sh  (root, seclabel su)
      MODEM/WDSP aus den eigenen Partitionen nach /proc/cpw, "start",
      wartet bis /dev/stty_w0 auf "AT" mit OK antwortet (max. ~60 s)
      → vendor.gtel3g.cp = running | failed | absent | off
 └─ on vendor.gtel3g.cp=running:
      vendor.gtel3g-nvitemd  (optional, Blob, persist.vendor.gtel3g.nvitemd=1)
      vendor.radio-gtel3g    = /vendor/bin/hw/android.hardware.radio-service.gtel3g
           AIDL radio.{config,data,messaging,modem,network,sim,voice} V5
           AT über /dev/stty_w0 (persist.vendor.radio.at_device)
           Daten über rmnet0..2 (seth), cid 1..3 → rmnet0..2
```

| Datei | Inhalt |
|---|---|
| `lineage_gtel3g.mk` | wie gtelwifi, aber `full_base_telephony.mk` + `common_mini_phone.mk` (Telefonie-Pakete, APNs aus vendor/lineage) |
| `device.mk` | erbt `gtelwifi/device.mk`; HAL-Paket, Telefonie-Features, Overlay, init/ueventd/Skript, Properties, optionale Blobs |
| `BoardConfig.mk` | (schon vorhanden) + `BOARD_VENDOR_SEPOLICY_DIRS` für das HAL-Label |
| `Android.bp` | `soong_namespace` (in `device.mk` exportiert) |
| `radio/` | der HAL (unten) |
| `rootdir/etc/init.gtel3g.rc` | → `/vendor/etc/init/` (wird automatisch gelesen; `init.gtelwifi.rc` gilt weiter, `ro.hardware` bleibt `gtelwifi`) |
| `rootdir/etc/ueventd.gtel3g.rc` | → `/vendor/etc/ueventd.gtelwifi.rc` (mainline/common importiert `ueventd.${ro.hardware}.rc`; gtelwifi hat keine eigene) |
| `rootdir/bin/gtel3g-modem.sh` | CP laden/starten, AT-Probe |
| `overlay/` | `config_voice_capable=false`, SMS und mobile Daten an |
| `sepolicy/vendor/file_contexts` | HAL-Binary als `rild_exec` (init braucht eine Domäne, auch permissive) |

## Der HAL (`radio/`)

Quelle: `device/generic/goldfish/hals/radio` aus dem LOS-24-Baum (Commit `c97eaa10`, Apache-2.0, `NOTICE`), Kopie
in `out/gtel3g-ril/goldfish-orig/radio/`. Jede Änderung im Code ist mit `gtel3g` markiert. Nicht übernommen:
`ImsMedia`, `RadioIms`, `Sap`, `EmulatorRadioConfig`, die Emulator-APNs/-rc. Keine goldfish-Namespaces oder -Defaults;
`Android.bp` ist eigenständig (eigenes `license`-Modul, gleiche `-V5-ndk`-Bibliotheken wie goldfish).

**AtChannel** (Kern der Anpassung):
- Gerätedatei statt Emulator-Socket, `open(O_RDWR|O_NOCTTY)`; termios nur falls `isatty()` und Fehler egal
  (`stty_w*` sind spipe-Knoten, kein tty [B `kernel/arch/arm/mach-sprd/sipc/spipe.c`]).
- Lesen mit `poll()` (500 ms), damit ein Reset nie in `read()` hängt; Zeilenbildung vor dem Parser: `\r\n`, leere
  Zeilen und Echo (`AT…`) weg, SMS-Prompt `> ` ohne Zeilenende wird ergänzt.
- **Ein Befehl gleichzeitig:** der nächste wartet (max. 3 s) auf das Final-Result des vorigen. Der CP schickt alle
  ~2,5 s unaufgefordert `+CSQ`/`+SPSCSQ` [B `dumps/gtel3g-a404/logcat.txt`]; ohne die Sperre könnte ein spätes `OK`
  die nächste Unterhaltung beenden.
- Unbekannte Zeilen (`+SPSCSQ`, `+SIND`, `+CGEV`, `^CARDMODE`, `+SIPCONFIG` …) werden zu `AtResponse::Unknown`
  statt den Kanal abzubauen (goldfish: `return -1` → Kanal zu). Eine Unterhaltung endet sofort mit `nullptr`, wenn
  ein Final-Result kommt, das der Filter nicht will (ERROR, +CME ERROR …), statt 3 s Timeout.
- Kanal-Reset nur bei I/O-Fehler oder 3 Timeouts in Folge ohne jede Zeile vom CP (goldfish: nach jedem
  fehlgeschlagenen Request; ERROR/+CME ERROR sind beim CP normale Antworten). `AT+CFUN?` nur beim ersten Öffnen
  (sonst meldete ein Reopen `+CFUN: 0` bei laufendem Funk). Kein `LOG_ALWAYS_FATAL` beim Init; nach
  fehlgeschlagenem Öffnen/Init 5 s Pause.
- Emulator-Befehle werden lokal beantwortet: `+CTEC` (aktuelle Technik aus dem `<AcT>` von `+CREG`/`+CGREG`, unterstützt
  GSM+WCDMA), `+WRMP`, `+CCSS`, `+WSOS`, `+CUSATD?`, `+CEREG`. **[V]** Die bevorzugte Netzart geht dadurch nicht an
  den CP (Vendor-RIL: `AT^SYSCONFIG=<n>,3,2,4` [B String]).
- AT-Mitschnitt im Radio-Log mit `persist.vendor.radio.at_log=1` (wirkt beim nächsten Öffnen des Kanals).

**Parser** (`AtResponse.cpp`): `+COPS` mit Anführungszeichen/AcT/„nur Modus“ (nicht registriert), `+CREG`/`+CGREG`
gelöst und ungelöst mit/ohne AcT, `+CSQ: <rssi>,<ber>` (für GSM und WCDMA), `+CRSM` mit gequoteter Antwort,
`+CMT: [<alpha>],<len>`, `+CPIN: NOT READY`/`SIM PIN2`, neu `+CGACT`, `CONNECT`, `NO CARRIER`.

**Init-Folge** (`main.cpp`, nur `AT` und `ATE0Q0V1` müssen antworten): `ATE0Q0V1`, `+CMEE=1`, `+CREG=2`, `+CGREG=2`,
`+CMGF=0`, `+CSMS=1`, `+CNMI=3,2,2,1,1`, `+CGEREP=1,0`, `+CTZR=1`, `+CMOD=0`, `+COLP=0`, `+SFUN=2`, `+CFUN?` —
alle als Strings im Vendor-RIL `libreference-ril_sp.so` [B]. **Kein `AT+CSCS="HEX"`** (sonst kämen Netzbetreibernamen
hex-kodiert [V]).

**Funk an/aus:** `AT+SFUN=4` / `AT+SFUN=5` [B Strings im Vendor-RIL, Bedeutung V], Rückfall `AT+CFUN=1/0`. Danach
meldet der HAL allen Diensten `+CFUN: 1/0`. **SIM:** `AT+SFUN=2` im Init [V: SIM einschalten; ohne Funk/SIM-Start kam
`+CME ERROR: 14` = SIM busy, `cp-test.txt`]. SIM-busy/NOT READY → „present, nicht bereit“, alle 3 s neue
`simStatusChanged` (max. 2 min), dazu bei `+SIND`/`+ECIND`/`+CPIN`. Kartentyp per `AT^CARDMODE` [B String, Antwort V:
`1` = SIM, sonst USIM], nur **eine** App (keine RUIM/ISIM). ICCID aus EF_ICCID per `AT+CRSM=176,12258,0,0,10`.
`AT+CRSM` jetzt mit gequotetem Data und **Pfad** (goldfish gab die AID als Pfad). IMEI `AT+CGSN` [B], Baseband
`AT+CGMR` [B: `T561XXU0AQA2`]. PIN falsch und `AT+CPINR` unbekannt → `PASSWORD_INCORRECT` mit −1 Versuchen; nach richtiger PIN/PUK sofort
`simStatusChanged`.

**SMS:** PDU-Modus, `AT+CMGS=<len>` → Prompt → PDU+Ctrl-Z (ohne CR), 60 s Timeout, `+CMS ERROR` → `GENERIC_FAILURE`.
Eingang über `+CMT` (CNMI 3,2,2,1,1, Quittung `AT+CNMA=1/2` wie Vendor-RIL [B]).

**Daten** (Spreadtrum-Weg, belegt aus Vendor-RIL + phoneserver):
```
AT+CGDCONT=<cid>,"IP","<apn>","",0,0                     [B Format libreference-ril_sp.so]
AT+CGPCO=0,"<user>","<pw>",<cid>,<auth>   (nur mit User)   [B Format; auth-Nummern V]
AT+CGDATA="M-ETHER",<cid>   → OK oder CONNECT (90 s)       [B]
AT+SIPCONFIG=<cid>          → +SIPCONFIG: <cid>,<n>,"s","<ip>","s","<dns1>","<dns2>"
AT+CGPADDR=<cid>            (nur falls SIPCONFIG keine IP liefert, 27.007)
AT+CGACT=0,<cid>            (Abbau, auch bei Fehlern)       [B]
```
- `+SIPCONFIG`: Reihenfolge der Felder aus phoneservers Parser [B: Disassembly, `at_tok_nextint` ×2, `at_tok_nextstr`
  ×5; Feld 4 → `net.rmnet<n>.ip`, 6/7 → `dns1/2`]; was in Feld 2, 3, 5 steht ist [V]. Fällt die Zuordnung aus,
  nimmt der HAL die erste gültige IPv4 als Adresse, die nächsten als DNS. Ohne DNS: `persist.vendor.radio.dns_fallback`
  (Komma-Liste, Standard leer).
- Interface `rmnet<cid-1>` [B phoneserver `addr add %s dev %s%d` mit Index cid−1], MTU 1400 [B phoneserver].
- **seth = Ethernet-Rahmen** [B `kernel/arch/arm/mach-sprd/sipc/seth.c`: `alloc_netdev(…, ether_setup)`,
  `eth_hw_addr_random`, RX `eth_type_trans`, TX sendet `skb->data` samt Ethernet-Kopf]. Hinter rmnet antwortet niemand
  auf ARP; phoneserver setzte `arp off` [B]. Der HAL setzt `IFF_UP|IFF_NOARP` (Capability NET_ADMIN im rc), Adresse
  `/32`, Gateway `0.0.0.0` (on-link-Default-Route), DNS aus SIPCONFIG. Besser wäre `IFF_NOARP` direkt im Treiber
  (`netdev->flags |= IFF_NOARP` in `seth_probe`) – Kernel-Änderung, hier nicht gemacht.
- Nur IPv4 [V]: IPV4V6/IPV6-Profile bekommen einen IP-Kontext; IPv6 auf rmnet0–2 im rc abgeschaltet.
- `+CGEV: … DEACT` → `AT+CGACT?` → nicht mehr aktive Kontexte werden gemeldet und abgebaut.

**Sprache:** `RadioVoice` bleibt (Framework erwartet `IRadioVoice`), nur offensichtliche goldfish-Fehler behoben
(`AT+CUSD` mit `%s`, `AT+CLIR:`, `getMute` schickte `AT+CLCC`). `config_voice_capable=false`.

## Bauen (Soong, LOS-24-Baum)

- `lunch lineage_gtel3g-cp2a-userdebug` (`AndroidProducts.mk`). `device/samsung/gtel3g` muss im Baum liegen;
  **`lineageos/scripts/push-to-buildhost.sh` überträgt bisher nur `gtelwifi`** (und `lineageos/proprietary/` →
  `device/samsung/gtelwifi/proprietary/`) – gtel3g dort ergänzen (gehört der LineageOS-Session).
- Modul `android.hardware.radio-service.gtel3g` (Namespace `device/samsung/gtel3g`, in `device.mk` exportiert),
  VINTF-Fragment `radio/android.hardware.radio-service.gtel3g.xml` (radio.* Version 5, FCM 202604 nimmt 3–5 laut
  `out/gtel3g-ril/ANALYSE.md`).
- Blobs optional, **nur lokal**: `lineageos/proprietary/gtel3g/bin/nvitemd`, `…/lib/libstdc++.so` (aus
  `dumps/gtel3g-rom-20260821/`, nie ins Git/Export). Ohne sie baut das Image; init loggt dann nur, dass nvitemd fehlt.

## Testanleitung (Tester mit SIM, T561)

Vorher: SIM **ohne PIN** (am einfachsten), Daten-Tarif, APN bekannt. Logs enthalten IMSI/ICCID/Rufnummern →
**vor dem Teilen schwärzen**.

1. Boot, dann `adb root`:
   ```
   getprop vendor.gtel3g.cp          # running?  (failed/absent: Kernel/Partitionen)
   logcat -d -s gtel3g-modem         # Skript-Log
   getprop init.svc.vendor.radio-gtel3g   # running?
   ```
2. AT-Mitschnitt an und HAL neu starten:
   ```
   setprop persist.vendor.radio.at_log 1
   stop vendor.radio-gtel3g; start vendor.radio-gtel3g
   logcat -b radio -v time -s radio-gtel3g RILJ > radio.txt     # „AT>“/„AT<“-Zeilen
   ```
3. SIM/Netz: `dumpsys telephony.registry | grep -iE "mServiceState|mSignalStrength|mDataConnectionState"`,
   `dumpsys isub | grep -i iccid`, Einstellungen → Netzwerk → SIM-Status. Erwartet: `AT+CPIN?` → `READY`,
   `+CREG: 2,1,…` / `+CGREG: …,1` (5 = Roaming), Betreibername aus `+COPS`.
4. Funk: `cmd phone radio power-state` [V Befehl], ggf. Flugmodus aus/an. Im AT-Log `AT+SFUN=4` → `OK`?
5. Daten: Mobile Daten an (`svc data enable`), APN in den Einstellungen prüfen/anlegen. Dann
   `ip addr show rmnet0`, `ip route show table all | grep rmnet`, `ping -c3 8.8.8.8`, `ping -c3 google.com`.
   Im AT-Log die `+SIPCONFIG:`-Zeile ansehen (Feldbelegung bestätigen!).
6. SMS: aus der Messaging-App an die eigene Nummer; im AT-Log `AT+CMGS` → `>` → `+CMGS: n`; Eingang als `+CMT`.
7. Direkter AT-Test ohne HAL: `stop vendor.radio-gtel3g`, dann wie `out/gtel3g-modem/cp-test.sh` (cat im Hintergrund,
   `printf 'AT+CPIN?\r' > /dev/stty_w0`), danach `start vendor.radio-gtel3g`.

Schalter: `persist.vendor.gtel3g.modem=0` (CP gar nicht starten), `persist.vendor.gtel3g.nvitemd=0` (ohne nvitemd),
`persist.vendor.radio.at_device` (anderer Kanal, z. B. `/dev/stty_w1`), `persist.vendor.radio.dns_fallback`.

## Ungetestet / Annahmen [V]

- Alles: weder gebaut noch gelaufen. Kompiliert wurden nur `AtChannel.cpp`, `AtResponse.cpp`, `gtel3gUtils.cpp`,
  `hexbin.cpp` (NDK-clang 19, `-fsyntax-only`, Stub-Header); `AtChannel`+Parser liefen auf dem Mac gegen ein
  simuliertes Modem (Echo, `+SPSCSQ` dazwischen, COPS mit `+CSQ` mittendrin, CMT, SMS-Prompt, CGDATA/CONNECT,
  SIPCONFIG, CRSM, ERROR → sofort `nullptr`). `Radio*.cpp` nur gelesen, nicht kompiliert (keine AIDL-Header lokal).
- `AT+SFUN=2/4/5`-Bedeutung, Antwortformat `^CARDMODE`, `+SIPCONFIG`-Felder 2/3/5, Auth-Nummern von `+CGPCO`,
  ob `AT+CGDATA="M-ETHER"` mit `OK` oder `CONNECT` endet, ob der CP `+CPINR` kennt.
- Ob der CP ohne `nvitemd` (NV-Dienst über `spipe_w1`) Funk macht; ob der LOS-16-`nvitemd` unter Android 17 startet
  (libstdc++/libhardware_legacy im Vendor-Namespace).
- Ob sboot MODEM/WDSP schon vorlädt (Skript lädt immer neu, wie der Test).
- Kein Watchdog/Assert-Neustart des CP (`/proc/cpw/wdtirq`, `spipe_w2`) – stürzt der CP ab, hilft nur Neustart.
- Nur IPv4, nur ein Kontext gleichzeitig sinnvoll getestet; bevorzugte Netzart wird ignoriert; kein STK, keine
  Netzsuche (`getAvailableNetworks` = not supported, wie goldfish).
- Rechte: HAL läuft als `radio` in der `rild`-Domäne (permissive); `/dev/stty_w*` radio:radio 0660 über ueventd.

## Was beim ersten Build vermutlich bricht

1. **Produktdateien:** `vendor/lineage/config/common_mini_phone.mk` (Name prüfen) und die Feature-XMLs
   `android.hardware.telephony.{radio.access,subscription,data,messaging}.xml` in `frameworks/native/data/etc`.
2. **Compiler bei `Radio*.cpp`:** ungeprüft gegen die echten NDK-Header: `CellIdentity::noinit`, `AppStatus::appType`,
   `DataProfileInfo::{user,password,authType}`, `IccIo::path`, Initialisierer von `LinkAddress`. `-Werror`-Warnungen
   aus goldfish (Vorzeichenvergleich in CSIM/CGLA) falls der Pfad strenger gebaut wird.
3. **VINTF:** falls die FCM 202604 andere radio-Versionen als 5 verlangt (dann `-Vn-ndk` und XML anpassen) oder
   `IRadioIms`/`ISap` doch Pflicht sind.
4. **SELinux/init:** `rild_exec`-Label (Typ muss in system/sepolicy existieren), `seclabel u:r:su:s0` (nur
   userdebug), `ueventd.gtelwifi.rc`-Import von mainline/common.
5. **Overlay:** `config_mobile_data_capable` existiert evtl. nicht mehr (dann Zeile löschen).
