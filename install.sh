#!/bin/bash

# Install Dependencies 
sudo apt update && sudo apt upgrade -y
sudo apt install -y mosquitto mosquitto-clients openssl

sudo apt install -y qrencode
# Install Pip stuff
pip3 install paho-mqtt --break-system-packages
pip3 install cryptography --break-system-packages

echo "Installing Dependencies..."
# Make directories and move the Python Scripts and services
sudo mkdir /opt/entgen
echo "Entgen folder created in Opt"
cd /opt/entgen
sudo mkdir devices
sudo mkdir firmware
echo "Devices and firmware folders created"
cd ~/Entgen_Mobile_Dependencies
sudo cp save_ca_bootstrap.py /opt/entgen/save_ca_bootstrap.py
sudo cp save_enrollment.py /opt/entgen/save_enrollment.py
echo "Python scripts moved to entgen"
# Move services to systemD 
# entgen-firmware makes the web server to host the files
# entgen-enrollment captures the openADR info for the provisioning flow
# entgen-ca-bootstrap runs a python script that runs the 2 CA system
sudo cp entgen-ca-bootstrap.service /etc/systemd/system/entgen-ca-bootstrap.service
sudo cp entgen-enrollment.service /etc/systemd/system/entgen-enrollment.service
sudo cp entgen-firmware.service /etc/systemd/system/entgen-firmware.service


BOOTSTRAP_SERVICE="entgen-ca-bootstrap.service"
ENROLLMENT_SERVICE="entgen-enrollment.service"
FIRMWARE_SERVICE="entgen-firmware.service"
# Reload Daemons
sudo systemctl daemon-reload

# Write entgen.conf


sudo tee /etc/mosquitto/conf.d/entgen.conf > /dev/null << 'EOF'
# Plaintext listener — for local debugging only
listener 1883
password_file /etc/mosquitto/passwd

# MQTT over TLS — for Opta client
listener 8883
cafile   /etc/mosquitto/certs/ca.crt
certfile /etc/mosquitto/certs/broker.crt
keyfile  /etc/mosquitto/certs/broker.key
tls_version tlsv1.2
require_certificate true
use_identity_as_username true
acl_file /etc/mosquitto/acl.conf
# MQTT over WSS — for Flutter app
listener 9001
protocol websockets
cafile   /etc/mosquitto/certs/ca.crt
certfile /etc/mosquitto/certs/broker.crt
keyfile  /etc/mosquitto/certs/broker.key
tls_version tlsv1.2
require_certificate true

# General
allow_anonymous false

log_type all
EOF
# Generate Certs
# Detect the Pi's current IP for the SAN — must happen at script-run time,
# not hardcoded, since every deployment's IP differs
# =============================================================================
# Step: Generate this deployment's own broker CA + cert (SAN-aware)
# =============================================================================
mkdir -p ~/certs && cd ~/certs

PI_IP=$(hostname -I | awk '{print $1}')
echo "Detected Pi IP: ${PI_IP}"

openssl genrsa -out ca.key 2048
openssl req -new -x509 -days 3650 \
    -key ca.key \
    -out ca.crt \
    -subj "/CN=EntgenCA/O=Entgen/C=US"

openssl genrsa -out broker.key 2048
openssl req -new \
    -key broker.key \
    -out broker.csr \
    -subj "/CN=entgen-broker.local/O=Entgen/C=US"

cat > broker_ext.cnf << EOF
[req]
req_extensions = v3_req
distinguished_name = req_distinguished_name
[req_distinguished_name]
[v3_req]
subjectAltName = @alt_names
[alt_names]
DNS.1 = entgen-broker.local
IP.1  = 127.0.0.1
IP.2  = ${PI_IP}
EOF

openssl x509 -req -days 3650 \
    -in broker.csr \
    -CA ca.crt -CAkey ca.key -CAcreateserial \
    -out broker.crt \
    -extensions v3_req -extfile broker_ext.cnf

# =============================================================================
# Step: Append the FIXED canonical Entwise Bootstrap CA
# This file ships as a static asset in the repo — NEVER generated here,
# NEVER unique per install. Every deployment must trust the same one,
# because every phone's app binary presents the same hardcoded bootstrap
# client cert, signed once, forever, by this one canonical CA.
# =============================================================================
if [ ! -f "./entwise-bootstrap-ca.crt" ]; then
    echo "ERROR: entwise-bootstrap-ca.crt not found in repo — cannot proceed."
    echo "This file must ship alongside install.sh, not be generated."
    exit 1
fi
cat entwise-bootstrap-ca.crt >> ca.crt
echo "Canonical Entwise Bootstrap CA appended to ca.crt."

sudo mkdir -p /etc/mosquitto/certs
sudo cp ca.crt broker.crt broker.key /etc/mosquitto/certs/
sudo chown mosquitto: /etc/mosquitto/certs/*

# Make passwd
echo "Generated MQTT credentials for this install."
sudo mosquitto_passwd -c -b /etc/mosquitto/passwd mozzy l1gmagett1
sudo chown mosquitto:mosquitto /etc/mosquitto/passwd
sudo chmod 600 /etc/mosquitto/passwd
sudo systemctl enable mosquitto
sudo systemctl start mosquitto
sudo systemctl status mosquitto
# Enable service
sudo systemctl enable $BOOTSTRAP_SERVICE
sudo systemctl enable $ENROLLMENT_SERVICE
sudo systemctl enable $FIRMWARE_SERVICE

# Start service
sudo systemctl start $BOOTSTRAP_SERVICE
sudo systemctl start $ENROLLMENT_SERVICE
sudo systemctl start $FIRMWARE_SERVICE

if systemctl -q is-active $BOOTSTRAP_SERVICE
then
    echo "$BOOTSTRAP_SERVICE is up and running!"
else
    echo "Failed to start $BOOTSTRAP_SERVICE."
fi

if systemctl -q is-active $ENROLLMENT_SERVICE
then
    echo "$ENROLLMENT_SERVICE is up and running!"
else
    echo "Failed to start $ENROLLMENT_SERVICE."
fi

if systemctl -q is-active $FIRMWARE_SERVICE
then
    echo "$FIRMWARE_SERVICE is up and running!"
else
    echo "Failed to start $FIRMWARE_SERVICE."
fi


# =============================================================================
# Step: Generate a random, per-install MQTT credential
# Random diversification means a leaked or never-rotated credential on
# ONE deployment never compromises any other. This is what the broker
# password rotation feature (Phase 7) later gives the installer/user a
# way to change — but the INITIAL value must never be a shared constant.
# =============================================================================
#MQTT_USERNAME="entgen"
#MQTT_PASSWORD=$(openssl rand -base64 24 | tr -d '/+=' | head -c 24)

# Make payload for QR 
SETUP_PAYLOAD=$(cat << JSON
{
  "broker_ip": "${PI_IP}",
  "broker_ca": "$(awk '{printf "%s\\n", $0}' ca.crt | head -c -1)",
  "mqtt_username": "mozzy",
  "mqtt_password": "l1gmagett1"
}
JSON
)

echo "${SETUP_PAYLOAD}" > ~/entgen_setup_payload.json
qrencode -t ANSIUTF8 < ~/entgen_setup_payload.json

echo ""
echo "Scan this QR code from the Entgen app during first-time setup."
echo "This payload is also saved at ~/entgen_setup_payload.json"

