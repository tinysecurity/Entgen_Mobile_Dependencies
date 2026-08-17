import paho.mqtt.client as mqtt
import json
import os

BROKER   = 'localhost'
PORT     = 1883
USERNAME = 'mozzy'
PASSWORD = 'l1gmagett1'
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