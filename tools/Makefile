CFLAGS ?= -O2 -g -Wall -Wextra
CFLAGS += $(shell pkg-config --cflags libusb-1.0)
LDLIBS += $(shell pkg-config --libs libusb-1.0) -lm

gip_probe: gip_probe.c

clean:
	rm -f gip_probe

.PHONY: clean
