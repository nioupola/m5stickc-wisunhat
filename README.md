# m5stickc-wisunhat

![https://img.shields.io/badge/hadware-m5stickc-blue](https://img.shields.io/badge/hadware-m5stickc-blue)

_Original work from [ak1211/m5stickc-wisunhat](https://github.com/ak1211/m5stickc-wisunhat)._

C++ code for M5StickCPlus (ex [shop.m5stack.com](https://shop.m5stack.com/products/m5stickc-plus-esp32-pico-mini-iot-development-kit)) to read SmartMeter using [B-Route](https://www.tepco.co.jp/pg/consignment/liberalization/smartmeter-broute.html) and publish the data to a MQTT broker (aka HomeAssistant with Mosquitto broker). Internet is only required to sync time.

You will need the Wi-SUN HAT (ex [ssci.to/7612](ssci.to/7612), doesn't come with the chip) and apply online to get the ID and password to connect to the SmartMeter.

The original code uses AWS IoT to send, store and compute the data. I wanted to avoid 3rd parties and internet connection as much I can. I added a action for the B bouton to turn off the screen to reduce consumption.

## Configuration

### M5Stick

You will need to create a file `settings.json` under `data/` using the following template:

```json
{
  "wifi": {
    "SSID": "**********",
    "password": "**********"
  },
  "RouteB": {
    "id": "ABCDEFGHIJKLMNOPQRSTUVWXYZ012345",
    "password": "0123456789AB"
  },
  "DeviceId": "SmartMeter",
  "SensorId": "SmartMeter",
  "MQTT": {
    "Endpoint": "HA_IP",
    "user": "MQTT_USER",
    "password": "MQTT_PASSWORD"
  }
}
```

### Home Assistant

#### MQTT

You will need to create a user and password inside Mosquitto broker since anonymous connexion is not allowed. To do so, you can go to `addon/core_mosquitto/config` and add inside `Logins`:

```yaml
- username: MQTT_USER
  password: MQTT_PASSWORD
```

#### Sensor

Since it's not an native HA integration, you have to configure the sensors to show and use the data. You can do so using the `File editor` and add the content to `configuration.yaml`:

```yaml
mqtt:
  sensor:
    - state_topic: "SmartMeter/Power/Instantaneous"
      unique_id: instantaneous_power
      name: "Instantaneous Power"
      device_class: power
      unit_of_measurement: W
      state_class: measurement
      icon: mdi:flash
    - state_topic: "SmartMeter/Energy/Cumulative/Positive"
      unique_id: cumulative_energy
      name: "Cumulative Energy Positive"
      device_class: energy
      unit_of_measurement: kWh
      state_class: total_increasing
      icon: mdi:flash
```

## Upload

If you use VSCode, you can add [PlateformIO](https://platformio.org/install/ide?install=vscode) to manage the code. It will download all the librairies and allow you to perform everything from VSCode.

You will have to upload the data first (`m5stick-c/Plateform/Build Filesystem Image` and `m5stick-c/Plateform/Upload Filesystem Image`) and the, the code (`m5stick-c/General/Upload`). You can use `Upload and Monitor` first to see log and debug if needed.
