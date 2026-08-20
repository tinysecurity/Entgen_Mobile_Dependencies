#!/bin/bash

# Install Dependencies 
sudo apt update && sudo apt upgrade -y
sudo apt install -y mosquitto mosquitto-clients openssl

sudo apt install -y qrencode
# Install Pip stuff
sudo pip3 install paho-mqtt python-etcd --break-system-packages
sudo pip3 install cryptography --break-system-packages

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
echo "--------------ENTGEN CONF ADDED--------------------"
sudo tee /etc/mosquitto/acl.conf > /dev/null << 'EOF'
# Bootstrap identity — permanently restricted, write-only, one topic.
# CN comes from the shared bootstrap cert already in ca_bootstrap_service.dart.
user entgen-flutter-client
topic write entgen/bootstrap/ca-delivery
# No read grants at all — this identity cannot even see the ack it
# triggers. It does not need to; ensureCaDelivered() reads the ack using
# the SAME bootstrap connection's own subscription, which is a normal
# client-side subscribe, not a broker-granted read on someone else's data.
# Actually: the bootstrap client DOES need to subscribe to its own ack
# topic to receive it. Add:
topic read entgen/bootstrap/+/ack

# Everything else — real per-device identities (entgen-device-*) —
# unrestricted for now (matches existing behavior). Per-device ACL
# scoping (a device only touching its own topics) is a separate,
# future hardening item, not part of this specific fix.
pattern readwrite entgen/#
pattern read $SYS/broker/uptime
EOF

echo "----------------ACL CONF ADDED-------------------------"
sudo chown mosquitto:mosquitto /etc/mosquitto/acl.conf
sudo chmod 0700 /etc/mosquitto/acl.conf
# Generate Certs
# Detect the Pi's current IP for the SAN — must happen at script-run time,
# not hardcoded, since every deployment's IP differs
# =============================================================================
# Step: Generate this deployment's own broker CA + cert (SAN-aware)
# =============================================================================
mkdir -p ~/certs && cd ~/certs

PI_IP=$(ip -4 addr show wlan0 | grep -oP '(?<=inet\s)\d+(\.\d+){3}')
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

cd ~/Entgen_Mobile_Dependencies

if [ ! -f "./entwise-bootstrap-ca.crt" ]; then
    echo "ERROR: entwise-bootstrap-ca.crt not found in repo — cannot proceed."
    echo "This file must ship alongside install.sh, not be generated."
    exit 1
fi

cat entwise-bootstrap-ca.crt >> ca.crt
echo "Canonical Entwise Bootstrap CA appended to ca.crt."

sudo mkdir -p /etc/mosquitto/certs
cd ~/certs
sudo cp ca.crt broker.crt broker.key /etc/mosquitto/certs/
sudo chown mosquitto: /etc/mosquitto/certs/*

# Make passwd
echo "Generated MQTT credentials for this install."
MQTT_USERNAME="entgen"
MQTT_PASSWORD=$(openssl rand -base64 24 | tr -d '/+=' | head -c 24)

sudo mosquitto_passwd -c -b /etc/mosquitto/passwd "${MQTT_USERNAME}" "${MQTT_PASSWORD}"

sudo chmod 640 /etc/mosquitto/passwd
sudo chgrp mosquitto /etc/mosquitto/passwd
sudo chown root /etc/mosquitto/passwd
INSTALL_USER="${SUDO_USER:-$(whoami)}"
sudo tee /opt/entgen/mqtt_credentials.env > /dev/null << EOF
MQTT_USERNAME=${MQTT_USERNAME}
MQTT_PASSWORD=${MQTT_PASSWORD}
EOF
sudo chmod 600 /opt/entgen/mqtt_credentials.env
sudo chown root:root /opt/entgen/mqtt_credentials.env
sudo chown "${INSTALL_USER}:${INSTALL_USER}" /opt/entgen/mqtt_credentials.env
sudo systemctl enable mosquitto
sudo systemctl start mosquitto
sudo systemctl status mosquitto --no-pager

# Enable service
sudo systemctl enable $BOOTSTRAP_SERVICE
sudo systemctl enable $ENROLLMENT_SERVICE
sudo systemctl enable $FIRMWARE_SERVICE

# Start service
sudo systemctl start $BOOTSTRAP_SERVICE
sudo systemctl start $ENROLLMENT_SERVICE
sudo systemctl start $FIRMWARE_SERVICE
sudo systemctl restart mosquitto 
if systemctl -q is-active $BOOTSTRAP_SERVICE
then
    echo "$BOOTSTRAP_SERVICE RUNNING!"
else
    echo "Failed to start $BOOTSTRAP_SERVICE."
fi

if systemctl -q is-active $ENROLLMENT_SERVICE
then
    echo "$ENROLLMENT_SERVICE RUNNING!"
else
    echo "Failed to start $ENROLLMENT_SERVICE."
fi

if systemctl -q is-active $FIRMWARE_SERVICE
then
    echo "$FIRMWARE_SERVICE RUNNING!"
else
    echo "Failed to start $FIRMWARE_SERVICE."
fi


# Make payload for QR — built with Python's json module, not shell string
# munging, so newlines inside the multi-line CA cert get properly escaped
# as \n rather than landing as literal, JSON-breaking line breaks.
python3 -c "
import json

with open('ca.crt') as f:
    ca_pem = f.read()

payload = {
    'broker_ip':     '${PI_IP}',
    'broker_ca':     ca_pem,
    'mqtt_username': '${MQTT_USERNAME}',
    'mqtt_password': '${MQTT_PASSWORD}',
}

with open('${HOME}/entgen_setup_payload.json', 'w') as f:
    json.dump(payload, f)
"
echo "-----------PAYLOAD GENERATED------------"
cp ${HOME}/entgen_setup_payload.json /opt/entgen/firmware/entgen_setup_payload.json
qrencode -t ANSIUTF8 < ~/entgen_setup_payload.json
sudo cp ~/entgen_setup_payload.json /opt/entgen/firmware/entgen_setup_payload.json
echo ""
echo "Scan this QR code from the Entgen app during first-time setup."
echo "This payload is also saved at ~/entgen_setup_payload.json"

cd ~
echo "You can also join the broker to your Entgen App at http://${PI_IP}:8080/entgen_setup_payload.json"
echo "BROKER: ${MQTT_USERNAME}"
echo "PASS: ${MQTT_PASSWORD}"
echo "-------SETUP COMPLETE----------"

