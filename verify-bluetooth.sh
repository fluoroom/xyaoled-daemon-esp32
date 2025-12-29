#!/bin/sh
# Bluetooth verification script for Docker container

echo "==================================="
echo "Bluetooth Access Verification"
echo "==================================="
echo ""

# Check if running in container
if [ -f /.dockerenv ]; then
    echo "✓ Running inside Docker container"
else
    echo "⚠ Not running inside Docker container"
fi
echo ""

# Check D-Bus
echo "Checking D-Bus..."
if [ -S /var/run/dbus/system_bus_socket ]; then
    echo "✓ D-Bus system socket found"
else
    echo "✗ D-Bus system socket NOT found"
    echo "  Expected: /var/run/dbus/system_bus_socket"
fi
echo ""

# Check Bluetooth devices
echo "Checking Bluetooth devices..."
if command -v hciconfig >/dev/null 2>&1; then
    hciconfig
    if [ $? -eq 0 ]; then
        echo "✓ Bluetooth adapter found"
    else
        echo "✗ No Bluetooth adapter detected"
    fi
else
    echo "⚠ hciconfig command not available"
fi
echo ""

# Check USB devices
echo "Checking USB devices..."
if [ -d /dev/bus/usb ]; then
    echo "✓ USB devices accessible"
    ls -la /dev/bus/usb/ | head -10
else
    echo "✗ USB devices NOT accessible"
fi
echo ""

# Check sys/class/bluetooth
echo "Checking /sys/class/bluetooth..."
if [ -d /sys/class/bluetooth ]; then
    echo "✓ /sys/class/bluetooth accessible"
    ls -la /sys/class/bluetooth/
else
    echo "✗ /sys/class/bluetooth NOT accessible"
fi
echo ""

# Check bluez tools
echo "Checking Bluez tools..."
for tool in bluetoothctl hcitool hciconfig; do
    if command -v $tool >/dev/null 2>&1; then
        echo "✓ $tool available"
    else
        echo "✗ $tool NOT available"
    fi
done
echo ""

echo "==================================="
echo "Verification Complete"
echo "==================================="
