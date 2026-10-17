#!/bin/sh
# Remove the astro_a20 DKMS module from every kernel.
# Usage: sudo ./dkms-uninstall.sh
set -e

cd "$(dirname "$0")"

NAME=astro-a20
VERSION=$(sed -n 's/^PACKAGE_VERSION="\(.*\)"/\1/p' dkms.conf)

if [ "$(id -u)" -ne 0 ]; then
	echo "run as root: sudo $0" >&2
	exit 1
fi

if dkms status -m "$NAME" -v "$VERSION" | grep -q .; then
	dkms remove -m "$NAME" -v "$VERSION" --all
fi
rm -rf "/usr/src/$NAME-$VERSION"

if lsmod | grep -q '^astro_a20 '; then
	rmmod astro_a20 || echo "astro_a20 is in use; it will be gone after a reboot"
fi

echo "Removed."
