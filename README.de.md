# capt-lbp – Linux-Treiber für Canon LBP2900 (CAPT)

*English version: [README.md](README.md)*

Freier CUPS-Treiber für den Canon **LBP2900** (USB-ID `04a9:2676`), einen
„GDI“-Laserdrucker, der nur Canons proprietäres CAPT-Protokoll spricht.
Er braucht keine Canon-Binärdateien, keinen `ccpd`-Daemon und keine 32-Bit-Bibliotheken.

## Funktionsweise

```
Dokument ──CUPS/cups-filters──▶ CUPS-Raster 600 dpi, 1 Bit ──▶ Backend "capt" ──USB──▶ LBP2900
                                                         (Hi-SCoA-Kompression,
                                                          bidirektionales CAPT)
```

* `ppd/Canon-LBP2900-capt.ppd` lässt CUPS jedes Dokument in 600-dpi-Schwarzweiß-Raster
  umwandeln und reicht es direkt an das Backend weiter.
* `capt` (C, nur libc) ist ein CUPS-Backend. Es findet den Drucker über sysfs, spricht über
  `/dev/usb/lpN` (Kernelmodul usblp) oder als Fallback direkt über usbfs, fragt den
  Druckerstatus ab, komprimiert die Seite bandweise und steuert den Seitenablauf.
* „Papier leer“: Die LED blinkt. Papier einlegen und die Taste am Drucker drücken, dann wird
  die Seite neu gedruckt.

## Installation (auf jedem Linux-PC)

Voraussetzungen: C-Compiler, `make`, CUPS, cups-filters, Ghostscript

| Distribution  | Pakete |
|---------------|--------|
| Debian/Ubuntu | `sudo apt install build-essential cups cups-filters ghostscript` |
| Fedora        | `sudo dnf install gcc make cups cups-filters ghostscript` |
| Arch          | `sudo pacman -S base-devel cups cups-filters ghostscript` |

```sh
sudo ./install.sh          # baut, installiert, legt Queue "Canon_LBP2900" an
lp -d Canon_LBP2900 tests/testpage.ps
```

Deinstallieren: `sudo ./install.sh --uninstall`

Manuell geht es auch: `make && sudo make install`. Danach den Drucker in den
Systemeinstellungen oder unter http://localhost:631 hinzufügen. Gerät „Canon LBP2900 (CAPT)“,
Treiber „Canon LBP2900, 1.0“.

## Optionen

* Papierformate: A4, A5, B5, Letter, Legal, Executive, Umschläge (C5, DL, #10, Monarch), benutzerdefiniert
* Medientyp: Normalpapier, Normalpapier leicht, dickes Papier (H), Folie
* Tonersparmodus: `lp -o captTonerSave=True …`

## Fehlersuche

* Das Backend schreibt das Protokoll des letzten Jobs nach `/var/log/capt-lbp.log`.
  Einen anderen Pfad legst du in `/etc/capt-lbp.conf` fest: `log=/pfad/zur/datei`.
* Erkennt das Backend den Drucker? `sudo /usr/lib/cups/backend/capt`
* Direkter Test ohne CUPS-Queue:
  ```sh
  gs -q -dSAFER -dNOPAUSE -dBATCH -sDEVICE=cups -r600 -dcupsColorSpace=3 \
     -dcupsBitsPerColor=1 -sPAPERSIZE=a4 -sOutputFile=/tmp/job.ras datei.pdf
  sudo DEVICE_URI="$(sudo /usr/lib/cups/backend/capt | awk '{print $2;exit}')" \
     /usr/lib/cups/backend/capt 1 user test 1 "" /tmp/job.ras
  ```
* Drucker reagiert nicht mehr: aus- und wieder einschalten.

## Entwicklung / Tests ohne Hardware

`make test` rendert eine Testseite und schickt sie an einen simulierten Drucker
(`tests/sim_printer.py`). Der Simulator dekodiert die Hi-SCoA-Daten mit einem unabhängigen
Decoder und vergleicht jede Seite pixelgenau mit dem Eingangsraster. Der zweite Lauf testet
den „Papier leer“-Ablauf.

## Herkunft & Lizenz

Das Wissen über das CAPT-Protokoll (Befehle, Statusbits, Hi-SCoA-Kompression) stammt aus
dem Reverse-Engineering des Projekts [captdriver](https://github.com/mounaiban/captdriver)
(Alexey Galakhov, Alexei Gordeev, Moses Chong u. a.). Der Kompressor erzeugt byte-identische
Ausgabe zu dessen Implementierung. Lizenz: GPL-3.0-or-later. Dies ist kein offizieller
Treiber von Canon.
