# Codec2-Sandbox und Grenzen

Der Dienst aktiviert vor dem Erzeugen von Binder- und Decoderthreads die
AOSP-Media-Minijail. Der Build installiert die eigene ARM-Variante der
AOSP-Richtlinie und die unveränderte Plattformvorlage zum Abgleich. Fehlt die Basisrichtlinie,
bricht AOSP den Dienststart ab. Die eigene Variante erlaubt zusätzlich nur
`mremap` mit `MREMAP_MAYMOVE`; die bereits erlaubte Kombination
`MREMAP_MAYMOVE | MREMAP_FIXED` bleibt erhalten. Der konkrete zusätzliche
Aufruf wurde bei der Einrichtung des minigbm-Ausgabepuffers aufgezeichnet.
Die Beschränkungen für gleichzeitig schreibbaren und ausführbaren Speicher
bleiben bestehen. Bedingte Regeln werden in einer Richtlinie ersetzt;
Minijail kann sie nicht durch Anhängen einer zweiten Regel überschreiben.

`/dev/sprd_vsp` erhält einen eigenen SELinux-Typ. Die Media-Domäne bekommt
Zugriff auf diesen Knoten und nur die vorhandenen ioctl-Nummern 1–10 und
32–35 der VSP-ABI. Die Policy erteilt keinen Zugriff auf generische
`device`-Knoten. Die laufende Prozesskennung, `NoNewPrivs`, `Seccomp` und
alle Workerthreads müssen bei der Hardwareprüfung kontrolliert werden.

Die Register-/DMA-Schnittstelle bleibt ein privilegierter Bring-up-Pfad.
Der Treiber verlangt `CAP_SYS_RAWIO`, weil die proprietäre Bibliothek DMA
direkt programmiert. Die SELinux-Erlaubnis dafür gilt nur für userdebug/eng;
Androids Einschränkung für User-Builds wird nicht aufgeweicht. Seccomp
ersetzt weder eine IOMMU noch eine validierende Kernel-Decoderschnittstelle.
Ein kompromittierter Decoder ist über diese Schnittstelle deshalb nicht
vollständig gegenüber dem Kernel isoliert.

Das Tablet läuft weiterhin global permissive. Ein erfolgreicher Seccomp-
Wiedergabetest und ein erfolgreicher SELinux-Build belegen keine vollständige
Enforcing-Abnahme. Eine produktive Sicherheitsfreigabe erfordert eine
gesonderte Lösung für den privilegierten Hardwarezugriff und einen
vollständigen Enforcing-Test.
