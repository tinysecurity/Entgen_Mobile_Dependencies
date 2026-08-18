#!/usr/bin/env python3
# =============================================================================
# /opt/entgen/save_ca_bootstrap.py
#
# Listens for one-time-per-phone CA bootstrap deliveries and appends them
# to the broker's trusted CA bundle — the Pi-side half of the trust
# bootstrap described in ca_bootstrap_service.dart.
#
# SECURITY NOTE — this script's own MQTT identity must be restricted by
# ACL to exactly one topic (see mosquitto ACL config, separate file).
# This script trusts the CONTENT of whatever arrives on that topic
# implicitly, by design — the ACL is what prevents anything except the
# narrow bootstrap client identity from ever reaching this topic at all.
# This script is not a security boundary; the ACL is.
#
# ORDERING NOTE — ack BEFORE restart, always:
#   reload_mosquitto() performs a full `systemctl restart mosquitto`
#   (a `reload` was tried first but does not reliably re-read cafile for
#   already-open TLS listeners — confirmed by direct testing). A restart
#   drops every currently connected client, including the phone's own
#   bootstrap connection that is sitting there waiting for the ack on
#   this exact topic. Publishing the ack AFTER restarting means the
#   phone has already been disconnected and never receives it, which
#   is precisely the bug that caused repeated 15s timeouts on the phone
#   despite the Pi-side append+restart completing successfully every
#   time. The ack must always be published first, before any restart.
#
# Same pattern as save_enrollment.py — paho-mqtt CallbackAPIVersion.VERSION2.
# =============================================================================

import paho.mqtt.client as mqtt
import json
import os
import subprocess
from cryptography import x509
from cryptography.hazmat.backends import default_backend

BROKER       = 'localhost'
PORT         = 1883   # loopback, plaintext — same as save_enrollment.py
# ── MQTT credentials — loaded from the install script's generated file,
# never hardcoded. Random per-deployment (see install.sh), so this must
# be read at runtime rather than baked into source. ────────────────────
CREDENTIALS_PATH = '/opt/entgen/mqtt_credentials.env'

def load_mqtt_credentials():
    creds = {}
    try:
        with open(CREDENTIALS_PATH) as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith('#') or '=' not in line:
                    continue
                key, _, value = line.partition('=')
                creds[key.strip()] = value.strip()
    except FileNotFoundError:
        print(f'FATAL: {CREDENTIALS_PATH} not found. Did install.sh run?')
        raise
    if 'MQTT_USERNAME' not in creds or 'MQTT_PASSWORD' not in creds:
        raise ValueError(f'{CREDENTIALS_PATH} missing MQTT_USERNAME or MQTT_PASSWORD')
    return creds['MQTT_USERNAME'], creds['MQTT_PASSWORD']

USERNAME, PASSWORD = load_mqtt_credentials()
BOOTSTRAP_TOPIC = 'entgen/bootstrap/ca-delivery'
CA_BUNDLE_PATH  = '/etc/mosquitto/certs/ca.crt'


def on_connect(client, userdata, flags, reason_code, properties):
    print(f'Connected to broker, rc={reason_code}')
    client.subscribe(BOOTSTRAP_TOPIC, qos=1)
    print(f'Subscribed to {BOOTSTRAP_TOPIC}')


def is_valid_pem_certificate(pem_str: str) -> bool:
    """
    Confirms the incoming payload is actually a parseable X.509 certificate
    before it's ever allowed near the live trust bundle. This is the check
    that was missing when a test payload's fake PEM content ("TESTDATA")
    got written straight into ca.crt and broke Mosquitto's TLS startup.
    Never trust the content of this topic — only the ACL that restricts
    who can reach it.
    """
    try:
        x509.load_pem_x509_certificate(pem_str.encode(), default_backend())
        return True
    except Exception:
        return False


def ca_already_trusted(new_pem: str) -> bool:
    """
    Dedupe check — if this exact CA cert is already in the bundle, skip
    re-appending it. Compares by exact PEM text match, which is sufficient
    here since CertificateService generates a fresh, distinct PEM per
    phone install; a genuine re-delivery from the same phone will be
    byte-identical.
    """
    if not os.path.exists(CA_BUNDLE_PATH):
        return False
    with open(CA_BUNDLE_PATH, 'r') as f:
        existing = f.read()
    return new_pem.strip() in existing


def reload_mosquitto():
    """
    Full restart — a `reload` (SIGHUP) does not reliably reopen TLS
    listener cert material for already-loaded cafile changes. Requires a
    narrowly-scoped sudoers entry allowing exactly this command, nothing
    broader. IMPORTANT: caller must ack the delivering client BEFORE
    calling this — see module docstring.
    """
    subprocess.run(
        ['sudo', '/usr/bin/systemctl', 'restart', 'mosquitto'],
        check=True,
    )

def on_message(client, userdata, msg):
    try:
        payload     = json.loads(msg.payload.decode())
        delivery_id = payload.get('delivery_id')
        ca_pem      = payload.get('ca_cert')

        if not delivery_id or not ca_pem:
            print('Bootstrap message missing delivery_id or ca_cert, ignoring.')
            return

        if not is_valid_pem_certificate(ca_pem):
            print(f'Rejected malformed CA cert (delivery_id={delivery_id}) — not writing to trust bundle.')
            return

        needs_append = not ca_already_trusted(ca_pem)

        if needs_append:
            with open(CA_BUNDLE_PATH, 'a') as f:
                f.write('\n')
                f.write(ca_pem.strip())
                f.write('\n')
            print(f'Appended new phone CA to {CA_BUNDLE_PATH} (delivery_id={delivery_id})')
        else:
            print(f'CA already trusted (delivery_id={delivery_id}), skipping append.')

        # ── Ack FIRST, always, regardless of which branch above ran ──────
        # This must happen before any restart. The phone's bootstrap
        # connection is sitting on this exact topic waiting for this
        # message; a restart after this point drops that connection but
        # the ack has already been safely delivered before it happens.
        ack_topic = f'entgen/bootstrap/{delivery_id}/ack'
        msg_info = client.publish(
            ack_topic,
            json.dumps({'status': 'trusted', 'delivery_id': delivery_id}),
            qos=1,
            retain=False,
        )
        msg_info.wait_for_publish(timeout=5)
        print(f'Published ack to {ack_topic}')

        # ── Restart only now, only if something actually changed on disk ──
        if needs_append:
            reload_mosquitto()
            print('Mosquitto restarted — new CA now trusted.')

    except Exception as e:
        print(f'Error processing CA bootstrap message: {e}')

client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
client.username_pw_set(USERNAME, PASSWORD)
client.on_connect = on_connect
client.on_message = on_message
client.connect(BROKER, PORT)
client.loop_forever()
