#!/bin/sh
# Install the astro_a20 module with DKMS for every kernel that has headers.
# Usage: sudo ./dkms-install.sh
set -e

cd "$(dirname "$0")"

NAME=astro-a20
VERSION=$(sed -n 's/^PACKAGE_VERSION="\(.*\)"/\1/p' dkms.conf)
SRC=/usr/src/$NAME-$VERSION

if [ "$(id -u)" -ne 0 ]; then
	echo "run as root: sudo $0" >&2
	exit 1
fi

if ! command -v dkms >/dev/null; then
	echo "dkms not found: install it first (Arch: pacman -S dkms)" >&2
	exit 1
fi

# replace an earlier registration of this version
if dkms status -m "$NAME" -v "$VERSION" | grep -q .; then
	dkms remove -m "$NAME" -v "$VERSION" --all
fi

rm -rf "$SRC"
mkdir -p "$SRC"
cp -r Makefile dkms.conf src "$SRC/"

dkms add -m "$NAME" -v "$VERSION"
for build in /lib/modules/*/build; do
	kernel=$(basename "$(dirname "$build")")
	[ -e "$build/Makefile" ] || continue
	dkms install -m "$NAME" -v "$VERSION" -k "$kernel"
done

echo
echo "Installed. The module now loads automatically whenever the transmitter"
echo "is plugged in, in game mode."
