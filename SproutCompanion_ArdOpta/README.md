# Sprout Companion Sketch

This sketch handles the WiFi, MQTT, and configuration plumbing for an Arduino
Opta running Sprout, so you can write your own control logic without needing
to understand networking, JSON parsing, or MQTT internals. Everything you need
to read and write is exposed through a small set of function calls — see
[Reading and Writing Data](#reading-and-writing-data) below.

- [Configuration](#configuration)
- [Installation](#installation)
- [How MQTT Works](#how-mqtt-works)
- [Topics](#topics)
- [Sprout's 12 Base Topics](#sprouts-12-base-topics)
- [Reading and Writing Data](#reading-and-writing-data)
- [Where to Add Your Own Code](#where-to-add-your-own-code)
- [Reconfiguration](#reconfiguration)
- [FAQ](#faq)

---

## Configuration

Sprout reads all of its settings from a single file, `config.json`, stored on
the Opta's flash. Here's what each field means and how to fill it in.

| Field | Meaning | How to find it |
|---|---|---|
| `wifi_ssid` | Your WiFi network's name | Check your router's admin page, or the label on the router itself |
| `wifi_password` | Your WiFi network's password | Same place as above |
| `mac` | The Opta's MAC address, as an array of 6 numbers (e.g. `[178, 244, 235, 162, 28, 36]`) | Printed on a label on the Opta, or readable via `Serial.println(WiFi.macAddress())` from a test sketch. Note: it must be entered as *decimal* numbers, not the hex pairs printed on the label — convert each hex pair (e.g. `B2`) to decimal (`178`) |
| `ip` | The static IP address you want this Opta to use | Pick an address on your local network that's outside your router's DHCP range, so nothing else can be handed the same address automatically. Check your router's DHCP settings page for that range |
| `subnet` | Your network's subnet mask | Almost always `255.255.255.0` for a typical home/office network |
| `gateway` | Your router's IP address | Usually printed on the router, or check "default gateway" in your computer's network settings |
| `dns` | A DNS server address | Your `gateway` address works for most home networks; `8.8.8.8` (Google's public DNS) is a reliable fallback |
| `broker_ip` | The IP address of the machine running your MQTT broker | If it's your laptop, find your laptop's IP with `ipconfig` (Windows) or `ifconfig`/`ip addr` (Mac/Linux). If it's a Raspberry Pi, check your router's list of connected devices |
| `broker_password` | The password for your MQTT broker, if it requires one | Set when you configure the broker — see [Setting Up a Broker for Testing](#setting-up-a-broker-for-testing) |
| `topics` | The list of MQTT topics this device uses | See [Sprout's 12 Base Topics](#sprouts-12-base-topics) below — you shouldn't need to edit this list unless you're adding custom topics |

**A note on `ip`:** static IPs prevent your Sprout device from silently
changing addresses after a router reboot, which would otherwise break your
`broker_ip`-based MQTT connection until you noticed and updated the config.

---

## Installation

### Manual Installation (Arduino IDE)

1. Install the Arduino IDE if you haven't already:
   [https://support.arduino.cc/hc/en-us/articles/360019833020-Download-and-install-Arduino-IDE](https://support.arduino.cc/hc/en-us/articles/360019833020-Download-and-install-Arduino-IDE)
2. Connect your Opta via USB and select it as the target board in the IDE.
3. **Partition the flash first, if this is a new/blank board.** Open the
   `QSPIFormat` example sketch (File → Examples) and run it once. This only
   needs to happen a single time per device.
4. Open `SproutCompanion.ino` and upload it to the Opta.
5. Load your `config.json` onto the device's flash filesystem (see below).

### Loading the Configuration File

The sketch expects `config.json` at `/fs/config.json` on the Opta's flash.
*(Detailed step-by-step instructions for transferring this file — currently
via a temporary provisioning sketch, eventually via the BLE commissioning
flow below — are still being finalized. Check back here, or ask in the
project's support channel, for the current recommended method.)*

### Commissioning (Coming Soon)

We're building a streamlined commissioning flow so you won't need the
Arduino IDE at all for day-to-day setup. The intended flow:

- **`setup_broker.sh`** *(stub — not yet implemented)*: a script for a
  Raspberry Pi Zero that installs and configures Mosquitto with Sprout's
  topic structure pre-created, so you have a working broker on your local
  network without manual setup.
- **`flash_via_ble.sh`** *(stub — not yet implemented)*: a script that
  pushes both the compiled sketch and your `config.json` to an Opta over
  Bluetooth Low Energy, without needing a USB connection to a computer
  running the Arduino IDE.

Until these ship, use the manual installation process above.

### Setting Up a Broker for Testing

For development, you can run an MQTT broker directly on your laptop using
Mosquitto:

1. Download Mosquitto for your platform:
   [https://mosquitto.org/download/](https://mosquitto.org/download/)
2. Install it and start the broker (on most platforms, running `mosquitto`
   from a terminal with default settings is enough to get started; consult
   the download page for platform-specific service setup).
3. Find your laptop's local IP address (`ipconfig` on Windows, `ifconfig`
   or `ip addr` on Mac/Linux) and use it as `broker_ip` in your config.
4. If you want password authentication (recommended even for local testing,
   and required before this is ever exposed beyond your own network), set
   one up with `mosquitto_passwd` — see Mosquitto's documentation for the
   exact command for your version.

You don't need to manually create the 12 Sprout topics ahead of time —
Mosquitto creates topics automatically the first time something publishes
or subscribes to them. Once your Opta boots with a valid `config.json`, it
will subscribe to and publish on all its configured topics automatically.
To watch this happen, you can subscribe to everything from a second terminal:

```
mosquitto_sub -h <broker_ip> -t "sprout/#" -v
```

---

## How MQTT Works

MQTT is a lightweight messaging protocol built around a simple idea:
instead of devices talking to each other directly, they all talk to a
central **broker**. A device that wants to send information **publishes**
a message to a named channel (a "topic"), and any device that wants to
receive that information **subscribes** to that same topic. The broker's
job is just to forward published messages to every subscriber — publishers
and subscribers never need to know about each other directly, or even be
online at the same time.

This makes MQTT well suited to devices like Sprout: your Opta can publish
sensor readings without knowing or caring whether a phone app, a dashboard,
or nothing at all is currently listening. Messages are typically small and
sent frequently, which keeps bandwidth and power use low — part of why MQTT
is a common choice for industrial and IoT devices, including ones on
constrained WiFi or cellular connections.

One additional feature worth knowing about: a **Last Will and Testament**
(LWT) is a message you register with the broker when you connect, which the
broker automatically publishes on your behalf *if your connection drops
unexpectedly* — without you having to send anything yourself. Sprout uses
this to let anything watching know when a device has gone offline
unexpectedly, rather than just going silent.

---

## Topics

A **topic** is just a named channel, written like a file path — for example
`sprout/status` or `sprout/input/button_1`. Anyone who subscribes to a topic
receives every message published to it. Topics don't need to be created or
registered in advance; publishing or subscribing to a topic that doesn't
exist yet simply brings it into existence. A single device can publish to
some topics and subscribe to others at the same time — Sprout does both.

---

## Sprout's 12 Base Topics

Every Sprout device ships with the same 12 topics, defined in `config.json`
and protected from being renamed or removed:

| Name | Topic | Type | Direction | Purpose |
|---|---|---|---|---|
| `device_status` | `sprout/status` | String | Publish (+ Last Will) | Reports `"online"`/`"offline"` |
| `reconfiguration` | `sprout/reconfiguration` | String | Subscribe | Receives reconfiguration messages — see [Reconfiguration](#reconfiguration) |
| `input_button1` | `sprout/input/button_1` | Boolean | Subscribe | An incoming on/off signal |
| `input_button2` | `sprout/input/button_2` | Boolean | Subscribe | An incoming on/off signal |
| `input_num1` | `sprout/input/number_1` | Float | Subscribe | An incoming numeric value |
| `input_num2` | `sprout/input/number_2` | Float | Subscribe | An incoming numeric value |
| `input_str` | `sprout/input/string_cmd` | String | Subscribe | An incoming text command |
| `output_status1` | `sprout/output/status_1` | Boolean | Publish | An outgoing on/off signal |
| `output_status2` | `sprout/output/status_2` | Boolean | Publish | An outgoing on/off signal |
| `output_num1` | `sprout/output/number_1` | Float | Publish | An outgoing numeric value |
| `output_num2` | `sprout/output/number_2` | Float | Publish | An outgoing numeric value |
| `output_str` | `sprout/output/string_log` | String | Publish | An outgoing text/log message |

---

## Reading and Writing Data

You never need to touch MQTT, JSON, or the topic table directly. Use these
functions from your own code:

**Reading incoming values:**
```cpp
bool  sproutButton1();
bool  sproutButton2();
float sproutInputNum1();
float sproutInputNum2();
const char* sproutInputStr();
```

**Sending outgoing values** (each of these also marks the value for
publishing on the next pass through `loop()`):
```cpp
void sproutSetOutputStatus1(bool value);
void sproutSetOutputStatus2(bool value);
void sproutSetOutputNum1(float value);
void sproutSetOutputNum2(float value);
bool sproutSetOutputStr(const char* value);
bool sproutSetDeviceStatus(const char* value); // usually only needed for custom status messages
```

---

## Where to Add Your Own Code

Look for `userLoop()` near the bottom of the sketch. It runs once per pass
through `loop()`, after incoming MQTT messages have been processed and
before outgoing values are published — so anything you set inside it goes
out the same pass.

```cpp
void userLoop() {
  // Your code goes here. For example:
  if (sproutButton1()) {
    sproutSetOutputStatus1(true);
  }
}
```

Avoid `delay()` or other long blocking calls inside `userLoop()` — they'll
stall WiFi/MQTT servicing for the whole device, not just your own code.

---

## Reconfiguration

Sprout can be reconfigured remotely — without re-flashing the sketch — by
publishing a JSON message to the `reconfiguration` topic
(`sprout/reconfiguration`). This is intended to be driven by a companion
phone app, but the underlying mechanism is just an ordinary MQTT publish,
so it can be tested with any MQTT client, including `mosquitto_pub`.

A reconfiguration message only needs to include the fields you want to
change — anything omitted is left as-is. For example, to change just the
broker's IP address:

```json
{ "broker_ip": "192.168.1.50" }
```

To add or update a custom topic:
```json
{
  "topics": [
    {"name": "custom_temp", "topic": "sprout/custom/temp", "data_type": "FLOAT32",
     "init_value": 0, "pub_flag": true, "sub_flag": false, "is_will": false}
  ]
}
```

To remove a custom topic:
```json
{ "remove_topics": ["custom_temp"] }
```

**What's protected:** the 12 base topics listed above, and all top-level
connection fields (`wifi_ssid`, `wifi_password`, `broker_ip`,
`broker_password`, `mac`, `ip`, `gateway`, `subnet`, `dns`) can be
*updated* but never *deleted* — a reconfiguration message can't remove
them entirely. **Changing a custom topic's data type isn't done in a
single step** — remove the old topic and add the new one (optionally in
the same message) rather than trying to patch its type directly.

**Safety behavior:** every reconfiguration message is validated in full
before anything on the live device changes. If validation fails for any
reason, the message is rejected, a clear error is logged, and the device
keeps running on its last known good configuration — nothing is left in a
half-applied state.

---

## FAQ

**My Opta won't connect to WiFi. What do I check first?**
Double-check `wifi_ssid` and `wifi_password` for typos, and confirm `ip`
is outside your router's DHCP range (an address collision can cause
intermittent, confusing failures). The Serial monitor prints a specific
WiFi status code on failure — check that against the troubleshooting table
in the sketch's comments.

**My Opta connects to WiFi but not to MQTT.**
Confirm `broker_ip` is correct and that the broker is actually running and
reachable from the Opta's network. If your broker requires a username or
uses TLS, confirm `broker_password` (and any additional auth setup) matches
what the broker expects.

**Can I add more than the 12 base topics?**
Yes — either by hand-editing `config.json` before first boot, or via a
[reconfiguration](#reconfiguration) message once the device is running.

**What happens if I send a malformed reconfiguration message?**
It's rejected outright, with the reason logged over Serial. The device's
current configuration is left completely untouched.

**Is the reconfiguration channel secure?**
Anyone who can publish to `sprout/reconfiguration` on your broker can
change your device's WiFi and MQTT credentials. Restrict access to this
topic at the broker level (authentication, and ideally per-topic access
control) rather than relying on the topic name being obscure.

**What's the difference between an input and an output topic?**
Inputs (`sub_flag: true`) are things Sprout listens for — commands or
sensor data coming *in*. Outputs (`pub_flag: true`) are things Sprout
reports *out* — status, readings, or logs. A topic can technically be
both, but that's an advanced case with its own handling — see the sketch's
comments on `onReceive` handlers if you need it.

**Why did my configuration file fail to load at boot?**
The sketch rejects a config file outright rather than partially applying
it — check the Serial monitor at boot for the specific field or rule that
failed (missing required field, a topic name collision, more than one
topic marked as the Last Will, etc.).

**How long can my string values be?**
`init_value` and incoming/outgoing string payloads can be up to 511
characters. Longer values are rejected rather than silently truncated.

**Do I need to run QSPIFormat every time I update the sketch?**
No — only once, the very first time you use a blank/new Opta. Re-uploading
`SproutCompanion.ino` afterward doesn't touch the partition table or your
saved `config.json`.
