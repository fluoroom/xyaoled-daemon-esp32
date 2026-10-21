#!/bin/bash
# Build the QEMU variant of the firmware (simulated panel, emulated
# Ethernet) and run it in Espressif's QEMU, with the board's HTTP port
# forwarded to localhost:${PORT:-18001} and the serial log in
# build-qemu/serial.log. Ctrl-C (or kill) stops it.
#
#   QEMU=/path/to/qemu-system-riscv32 test/qemu/run.sh
#
# Get QEMU with `idf_tools.py install qemu-riscv32`, or unpack the
# qemu-riscv32 release that tools/tools.json lists.
set -euo pipefail
cd "$(dirname "$0")/../.."
: "${IDF_PATH:?run . \$IDF_PATH/export.sh first}"
PORT=${PORT:-18001}
QEMU=${QEMU:-qemu-system-riscv32}
B=build-qemu

idf.py -B $B -D SDKCONFIG=$B/sdkconfig -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.qemu" build >/dev/null
(cd $B && python -m esptool --chip esp32c3 merge-bin --pad-to-size 4MB -o flash.bin @flash_args >/dev/null)
# The eFuse image idf.py qemu would generate (chip revision 0.3).
python - "$B/efuse.bin" <<'PY'
import sys, os
sys.path.insert(0, os.path.join(os.environ['IDF_PATH'], 'tools'))
from idf_py_actions.qemu_ext import QEMU_TARGETS
open(sys.argv[1], 'wb').write(QEMU_TARGETS['esp32c3'].default_efuse)
PY

exec "$QEMU" -M esp32c3 -nographic \
	-drive file=$B/flash.bin,if=mtd,format=raw \
	-drive file=$B/efuse.bin,if=none,format=raw,id=efuse \
	-global driver=nvram.esp32c3.efuse,property=drive,value=efuse \
	-global driver=timer.esp32c3.timg,property=wdt_disable,value=true \
	-nic user,model=open_eth,hostfwd=tcp:127.0.0.1:$PORT-:11001 \
	-serial file:$B/serial.log -monitor none
