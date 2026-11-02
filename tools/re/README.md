# Reverse-Engineering-Helfer (sboot/SPL, HAL-Bibliotheken)

- `findref.py <SBOOT.img> <off>…`: Literal-Pool-Verweise auf Offsets in sboot (Ladeadresse `0x9F600000`).
- `movpairs.py <sboot.dis> <off>…`: MOVW/MOVT-Paare im Disassembly, die eine Adresse bilden.
- `elfsym.py`, `elfdump.py` (stdin = ELF): Symbole bzw. Sektionen von Android-Bibliotheken (z. B. `camera.sc8830.so`).

Disassembly liegt in `vendor-ref/disasm/` (neu erzeugbar, Befehl in CLAUDE.md).
