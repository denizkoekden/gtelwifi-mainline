# Testskripte fürs Tab (Labor)

Einmalige Messungen und Experimente, gesichert aus Session-Scratchpads. Laufen auf dem Tab
(`tools/nurassh 'cat > /var/tmp/x.sh' < tools/lab/x.sh`), die meisten brauchen ADI-debugfs
und als Sicherheitsnetz `tools/pmic-wdg.sh`.

| Datei | Zweck |
|---|---|
| `freqtest.sh` | cpufreq-Stufen einzeln setzen, Takt messen, Register lesen, sha256-Last gegen Referenz |
| `soak.sh` | 3 min Volllast auf allen Kernen mit schedutil, alle 10 s Takt/Temperatur |
| `popsteps.sh` | Lautsprecherpfad Schritt für Schritt einschalten (Knack-Suche), Schrittnummer auf fbcon |
| `popvar.sh`, `popvar2.sh` | PA-Ein/Aus-Varianten A–H (Soft-Start, Class D, Reihenfolge) |
| `gpu-on.sh` | GPU_TOP von Hand einschalten, Mali-Versionsregister lesen, wieder aus |
| `r8check.sh` | Schnellcheck nach einem Kernel-Update unter Nura (CFI, Warnungen, FGU …) |
| `susp.sh`, `susp5.sh` | Suspend-Einzeltest bzw. 5 Zyklen mit RTC-Wecken (Vorläufer von `tools/susp-test.sh`) |
| `pngstat.py` | PNG auf dem Tab auswerten (Größe, Mittelwerte), ohne Bild zu übertragen |
| `s5kdec.py` | S5K4ECGX-Tabellen (`0x0028`/`0x002a`/`0x0f12`) in Adresse/Wert dekodieren |
| `usbreset.c` | macOS: USB-Gerät per libusb neu enumerieren (half bei gesperrtem Mac nicht) |
