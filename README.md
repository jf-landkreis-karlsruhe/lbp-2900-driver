# capt-lbp – Linux driver for the Canon LBP2900 (CAPT)

*Deutsche Version: [README.de.md](README.de.md)*

A free CUPS driver for the Canon **LBP2900** (USB ID `04a9:2676`). This "GDI" laser
printer only speaks Canon's proprietary CAPT protocol.
The driver needs no Canon binaries, no `ccpd` daemon and no 32-bit libraries.

## How it works

```
document ──CUPS/cups-filters──▶ CUPS raster, 600 dpi, 1 bit ──▶ "capt" backend ──USB──▶ LBP2900
                                                            (Hi-SCoA compression,
                                                             bidirectional CAPT)
```

* `ppd/Canon-LBP2900-capt.ppd` makes CUPS render every document to 600 dpi black-and-white
  raster. CUPS then hands that raster unchanged to the backend.
* `capt` is a CUPS backend written in C that depends only on libc. It:
  * finds the printer via sysfs
  * talks to it through `/dev/usb/lpN` (the usblp kernel module), or directly via usbfs as a fallback
  * polls the printer status
  * compresses each page band by band
  * drives the page sequence
* Out of paper: the printer's LED blinks. Load paper and press the button on the printer, and the
  page is printed again.

## Status

| Feature | State |
|---------|-------|
| Printing via usblp, A4, multiple pages | ✅ verified on real hardware |
| Out-of-paper recovery (LED blinks, press button) | tested in the simulator only |
| usbfs fallback (no usblp) | tested in the simulator only |
| Paper sizes other than A4 | not tested on hardware yet |

Only the LBP2900 has been tested. Other CAPT printers (LBP3000, LBP3010, …) may need a different
command sequence.

## Installation (on any Linux PC)

Requirements: C compiler, `make`, CUPS, cups-filters, Ghostscript

| Distribution  | Packages |
|---------------|----------|
| Debian/Ubuntu | `sudo apt install build-essential cups cups-filters ghostscript` |
| Fedora        | `sudo dnf install gcc make cups cups-filters ghostscript` |
| Arch          | `sudo pacman -S base-devel cups cups-filters ghostscript` |

Connect the printer via USB and switch it on, then run:

```sh
sudo ./install.sh          # builds, installs, creates the queue "Canon_LBP2900"
lp -d Canon_LBP2900 tests/testpage.ps
```

Uninstall: `sudo ./install.sh --uninstall`

Manual installation: `make && sudo make install`. Then add the printer in your desktop's printer
settings or at http://localhost:631. Choose the device "Canon LBP2900 (CAPT)" and the driver
"Canon LBP2900, 1.0".

## Options

* Paper sizes: A4, A5, B5, Letter, Legal, Executive, envelopes (C5, DL, #10, Monarch), custom
* Media types: plain, plain (light), heavy, heavy H, transparency
* Toner save: `lp -o captTonerSave=True …`

## About usblp

The backend prefers the kernel's `usblp` driver. Some distributions blacklist it, historically
Debian/Ubuntu through their CUPS package. A blacklist only stops the module from loading
automatically, so `install.sh` loads it explicitly and registers it in
`/etc/modules-load.d/capt-lbp.conf`. If the module is missing entirely, the backend falls back to
raw USB via usbfs. To force that fallback, set `CAPT_TRANSPORT=usbfs`.

To check a machine:

```sh
lsmod | grep usblp; ls /dev/usb/lp*; grep -r usblp /etc/modprobe.d/
```

## Troubleshooting

* The backend logs the most recent job to `/var/log/capt-lbp.log`.
  To use a different file, put `log=/path/to/file` in `/etc/capt-lbp.conf`.
* Does the backend see the printer? Run `sudo /usr/lib/cups/backend/capt`
* Test directly, without a CUPS queue:
  ```sh
  gs -q -dSAFER -dNOPAUSE -dBATCH -sDEVICE=cups -r600 -dcupsColorSpace=3 \
     -dcupsBitsPerColor=1 -sPAPERSIZE=a4 -sOutputFile=/tmp/job.ras file.pdf
  sudo DEVICE_URI="$(sudo /usr/lib/cups/backend/capt | awk '{print $2;exit}')" \
     /usr/lib/cups/backend/capt 1 user test 1 "" /tmp/job.ras
  ```
* Printer stops responding: switch it off and on again.
* Streaky gray areas usually mean the toner cartridge is running low.

## Development / testing without hardware

`make test` renders a test page and sends it to a simulated printer (`tests/sim_printer.py`).
The simulator decodes the Hi-SCoA data with an independent decoder and compares every page
pixel by pixel with the input raster. A second run exercises the out-of-paper sequence.

Source layout:

| File | Purpose |
|------|---------|
| `src/backend.c` | CUPS backend: device discovery, job loop, cancel handling |
| `src/capt.c` | CAPT packets, status polling, job/page sequence |
| `src/hiscoa.c` | Hi-SCoA band compression |
| `src/raster.c` | CUPS/PWG raster reader (no libcups needed) |
| `src/transport.c` | sysfs discovery, usblp and usbfs I/O |
| `ppd/capt-lbp.drv` | PPD source; regenerate with `make ppd` (needs `ppdc`) |

## Credits & license

The CAPT protocol knowledge (commands, status bits, Hi-SCoA compression) comes from the
reverse-engineering work of the [captdriver](https://github.com/mounaiban/captdriver) project
(Alexey Galakhov, Alexei Gordeev, Moses Chong et al.). This driver's compressor produces output
that is byte-identical to captdriver's implementation.

License: GPL-3.0-or-later (see `LICENSE`). This is not an official Canon driver and is not
affiliated with or endorsed by Canon Inc.
