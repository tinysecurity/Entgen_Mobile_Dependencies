import paho.mqtt.client as mqtt
import json
import os

BROKER   = 'localhost'
PORT     = 1883

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
SAVE_DIR = '/opt/entgen/devices'

def on_connect(client, userdata, flags, reason_code, properties):
    print(f'Connected to broker, rc={reason_code}')
    client.subscribe('entgen/+/enroll', qos=1)
    print('Subscribed to entgen/+/enroll')

def on_message(client, userdata, msg):
    try:
        topic     = msg.topic
        device_id = topic.split('/')[1]
        payload   = json.loads(msg.payload.decode())

        path = os.path.join(SAVE_DIR, f'{device_id}.json')
        with open(path, 'w') as f:
            json.dump(payload, f, indent=2)

        print(f'Saved config for {device_id} to {path}')

        client.publish(
            f'entgen/{device_id}/enrolled',
            json.dumps({'status': 'saved', 'device_id': device_id}),
            qos=1,
            retain=True,
        )
        print(f'Published enrolled confirmation for {device_id}')

    except Exception as e:
        print(f'Error saving enrollment: {e}')

client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
client.username_pw_set(USERNAME, PASSWORD)
client.on_connect = on_connect
client.on_message = on_message
client.connect(BROKER, PORT)
client.loop_forever()
