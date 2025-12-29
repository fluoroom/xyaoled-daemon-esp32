#!/bin/bash

echo "Building XYAOLED Daemon..."
cd /home/nic0/projects/xyaoled-daemon
go mod tidy
go build -o xyaoled-daemon .

if [ $? -eq 0 ]; then
    echo "✓ Daemon built successfully: xyaoled-daemon"
    echo ""
    echo "To run:"
    echo "  cd /home/nic0/projects/xyaoled-daemon"
    echo "  ./xyaoled-daemon"
else
    echo "✗ Build failed"
    exit 1
fi
