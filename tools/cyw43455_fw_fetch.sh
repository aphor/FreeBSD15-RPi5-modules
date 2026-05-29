#!/bin/sh
# tools/cyw43455_fw_fetch.sh — download CYW43455 firmware from RPi firmware repo
#
# Downloads brcmfmac43455-sdio.bin and brcmfmac43455-sdio.txt into the
# working directory so that Makefile.cyw43455fw can embed them into the KLD.
#
# Usage:
#   cd ~/   # or wherever your rpi5_modules source files live
#   sh tools/cyw43455_fw_fetch.sh

set -e

BASE_URL="https://raw.githubusercontent.com/RPi-Distro/firmware-nonfree/master/debian/config/brcm"
BIN="brcmfmac43455-sdio.bin"
TXT="brcmfmac43455-sdio.txt"

echo "Fetching ${BIN}..."
fetch -q -o "${BIN}" "${BASE_URL}/${BIN}"
echo "  ${BIN}: $(wc -c < ${BIN} | tr -d ' ') bytes"

echo "Fetching ${TXT}..."
fetch -q -o "${TXT}" "${BASE_URL}/${TXT}"
echo "  ${TXT}: $(wc -c < ${TXT} | tr -d ' ') bytes"

echo "Done. Run: make -f Makefile.cyw43455fw"
