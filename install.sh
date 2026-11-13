#!/bin/sh
# Builds and installs the capt-lbp driver and sets up a CUPS print queue.
#   sudo ./install.sh            install + create queue "Canon_LBP2900"
#   sudo ./install.sh --uninstall
set -eu
cd "$(dirname "$0")"
QUEUE=${QUEUE:-Canon_LBP2900}

if [ "$(id -u)" != 0 ]; then
	echo "Bitte mit sudo ausführen: sudo $0 $*" >&2
	exit 1
fi

if [ "${1:-}" = "--uninstall" ]; then
	lpadmin -x "$QUEUE" 2>/dev/null || true
	make uninstall
	rm -f /etc/modules-load.d/capt-lbp.conf
	echo "Deinstalliert."
	exit 0
fi

for tool in cc make lpadmin gs; do
	command -v $tool >/dev/null 2>&1 || {
		echo "Fehlt: $tool  (Debian/Ubuntu: apt install build-essential cups ghostscript;" >&2
		echo "        Fedora: dnf install gcc make cups ghostscript; Arch: pacman -S base-devel cups ghostscript)" >&2
		exit 1; }
done
[ -x "$(cups-config --serverbin 2>/dev/null || echo /usr/lib/cups)/filter/gstoraster" ] || \
	[ -x /usr/lib/cups/filter/gstoraster ] || \
	echo "Warnung: cups-filters (gstoraster) scheint zu fehlen - bitte installieren." >&2

make
make install

# usblp kernel driver is preferred (blacklisted on some distros; usbfs fallback works too)
if modprobe usblp 2>/dev/null; then
	echo usblp > /etc/modules-load.d/capt-lbp.conf
fi

# restart CUPS so it sees the new backend
systemctl restart cups 2>/dev/null || service cups restart 2>/dev/null || true
sleep 2

SERVERBIN=$(cups-config --serverbin 2>/dev/null || echo /usr/lib/cups)
URI=$("$SERVERBIN/backend/capt" 2>/dev/null | awk '$2 ~ /^capt:\/\// {print $2; exit}')
if [ -z "$URI" ]; then
	echo "Drucker nicht gefunden (USB eingesteckt und eingeschaltet?)."
	echo "Queue später anlegen mit:  sudo QUEUE=$QUEUE $0"
	exit 0
fi

PPD="$(cups-config --datadir 2>/dev/null || echo /usr/share/cups)/model/capt-lbp/Canon-LBP2900-capt.ppd"
lpadmin -p "$QUEUE" -E -v "$URI" -P "$PPD" -D "Canon LBP2900" -L "USB" \
	-o printer-error-policy=abort-job -o PageSize=A4
cupsenable "$QUEUE" 2>/dev/null || true
cupsaccept "$QUEUE" 2>/dev/null || true
echo
echo "Fertig: Queue '$QUEUE' -> $URI"
echo "Testdruck:  lp -d $QUEUE tests/testpage.ps"
