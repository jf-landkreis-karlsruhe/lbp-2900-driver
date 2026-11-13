# capt-lbp - Linux/CUPS driver for Canon LBP2900 (CAPT)
CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=gnu11 -Wall -Wextra
LDFLAGS ?=

PREFIX      ?= /usr
CUPS_SERVERBIN ?= $(shell cups-config --serverbin 2>/dev/null || echo $(PREFIX)/lib/cups)
CUPS_DATADIR   ?= $(shell cups-config --datadir 2>/dev/null || echo $(PREFIX)/share/cups)
PPDDIR      ?= $(CUPS_DATADIR)/model/capt-lbp
DESTDIR     ?=

SRC = src/backend.c src/capt.c src/hiscoa.c src/raster.c src/transport.c src/log.c
HDR = src/capt.h

all: capt

capt: $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

# regenerate the PPD from its source (needs ppdc from the CUPS dev tools)
ppd: ppd/capt-lbp.drv
	ppdc -d ppd ppd/capt-lbp.drv

test: capt
	JOBS=2 sh tests/run_sim_test.sh ./capt
	sh tests/run_sim_test.sh ./capt tests/testpage.ps --small-buffer
	sh tests/run_sim_test.sh ./capt tests/testpage.ps --no-paper-once
	PAPER=a5 sh tests/run_sim_test.sh ./capt

install: capt
	install -d $(DESTDIR)$(CUPS_SERVERBIN)/backend $(DESTDIR)$(PPDDIR)
	# 0700 root: CUPS runs the backend as root (needed for raw USB access)
	install -m 0700 capt $(DESTDIR)$(CUPS_SERVERBIN)/backend/capt
	install -m 0644 ppd/Canon-LBP2900-capt.ppd $(DESTDIR)$(PPDDIR)/

uninstall:
	rm -f $(DESTDIR)$(CUPS_SERVERBIN)/backend/capt
	rm -rf $(DESTDIR)$(PPDDIR)

clean:
	rm -f capt

.PHONY: all ppd test install uninstall clean
