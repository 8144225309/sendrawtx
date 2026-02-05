#!/bin/bash
# RawRelay Installation Script
# Run as root: sudo ./contrib/install.sh

set -e

INSTALL_DIR="/opt/rawrelay"
SERVICE_USER="rawrelay"

echo "=== RawRelay Installation ==="

# Check root
if [ "$EUID" -ne 0 ]; then
    echo "Error: Run as root (sudo ./contrib/install.sh)"
    exit 1
fi

# Check binary exists
if [ ! -f "rawrelay-server" ]; then
    echo "Error: rawrelay-server not found. Run 'make' first."
    exit 1
fi

# Create user
if ! id "$SERVICE_USER" &>/dev/null; then
    echo "Creating user: $SERVICE_USER"
    useradd -r -s /bin/false -d "$INSTALL_DIR" "$SERVICE_USER"
fi

# Create directories
echo "Creating directories..."
mkdir -p "$INSTALL_DIR"
mkdir -p "$INSTALL_DIR/.well-known/acme-challenge"
mkdir -p "$INSTALL_DIR/data"
mkdir -p "$INSTALL_DIR/static"

# Copy files
echo "Installing files..."
cp rawrelay-server "$INSTALL_DIR/"
cp config.ini.example "$INSTALL_DIR/config.ini"
cp -r static/* "$INSTALL_DIR/static/" 2>/dev/null || true

# Set permissions
echo "Setting permissions..."
chown -R "$SERVICE_USER:$SERVICE_USER" "$INSTALL_DIR"
chmod 750 "$INSTALL_DIR"
chmod 755 "$INSTALL_DIR/rawrelay-server"
chmod 640 "$INSTALL_DIR/config.ini"

# Install systemd service
echo "Installing systemd service..."
cp contrib/init/rawrelay.service /etc/systemd/system/
systemctl daemon-reload

echo ""
echo "=== Installation Complete ==="
echo ""
echo "Next steps:"
echo "  1. Edit config:     sudo nano $INSTALL_DIR/config.ini"
echo "  2. Start service:   sudo systemctl start rawrelay"
echo "  3. Enable on boot:  sudo systemctl enable rawrelay"
echo "  4. Check status:    sudo systemctl status rawrelay"
echo "  5. View logs:       sudo journalctl -u rawrelay -f"
echo ""
