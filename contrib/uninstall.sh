#!/bin/bash
# RawRelay Uninstall Script
# Run as root: sudo ./contrib/uninstall.sh

set -e

INSTALL_DIR="/opt/rawrelay"
SERVICE_USER="rawrelay"

echo "=== RawRelay Uninstallation ==="

if [ "$EUID" -ne 0 ]; then
    echo "Error: Run as root (sudo ./contrib/uninstall.sh)"
    exit 1
fi

# Stop service
echo "Stopping service..."
systemctl stop rawrelay 2>/dev/null || true
systemctl disable rawrelay 2>/dev/null || true

# Remove systemd service
echo "Removing systemd service..."
rm -f /etc/systemd/system/rawrelay.service
systemctl daemon-reload

# Ask about data
read -p "Remove $INSTALL_DIR and all data? [y/N] " -n 1 -r
echo
if [[ $REPLY =~ ^[Yy]$ ]]; then
    rm -rf "$INSTALL_DIR"
    echo "Removed $INSTALL_DIR"
fi

# Ask about user
read -p "Remove user '$SERVICE_USER'? [y/N] " -n 1 -r
echo
if [[ $REPLY =~ ^[Yy]$ ]]; then
    userdel "$SERVICE_USER" 2>/dev/null || true
    echo "Removed user $SERVICE_USER"
fi

echo ""
echo "=== Uninstallation Complete ==="
echo "Note: Logs are in journald. Clear with: sudo journalctl --vacuum-time=1s -u rawrelay"
