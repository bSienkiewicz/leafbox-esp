## Leafbox ESP

#### Project map:
* **[Leafbox Web App](https://github.com/bSienkiewicz/leafbox)**
* ***Leafbox ESP32***
* **[Leafbox API](https://github.com/bSienkiewicz/leafbox-api)**
---

**Leafbox ESP** is a crucial component of the **Leafbox Project**, designed to function as a measurement and control unit for plant care. 
This device is programmed to monitor and manage the moisture levels of up to four plants simultaneously, sending data wirelessly to the MQTT broker and, if necessary, taking automated actions, such as powering water pumps or making measurements on demand.

![esp-photo](https://github.com/bSienkiewicz/leafbox-esp/assets/50502786/df9619b6-071a-4e92-8ada-fe48bea9b489)

### Features

- **Soil Moisture Measurement**: Monitors the soil moisture levels of up to 4 plants, ensuring optimal conditions.
- **Wireless Data Transmission**: Sends real-time moisture data to the MQTT broker over a wireless connection.
- **Automated Watering**: Controls up to 4 12V pumps or solenoid valves, activated through a 4-relay module, to water plants as needed.
- **Customizable Configurations**: Each plant's parameters are configured exclusively via the  **[Leafbox Web App](https://github.com/bSienkiewicz/leafbox)**.

## Schematic
![esp-schema](https://github.com/bSienkiewicz/leafbox-esp/assets/50502786/498b378f-4255-4f17-aba4-afcca619ba5f)


## Configuration Webpage

The ESP32 hosts a web-based configuration page that allows users to connect to the device's Wi-Fi network and set up network and MQTT broker details. Below is a screenshot of the configuration page:

![esp-config](https://github.com/bSienkiewicz/leafbox-esp/assets/50502786/460d7bc9-1aba-4d84-862a-447b71934bcb)


### How it Works

- The **Leafbox ESP** unit monitors the soil moisture levels of connected plants using connecter water moisture sensors of user's choice. 
- It transmits this data to the MQTT broker, which then communicates with the **[Leafbox API](https://github.com/bSienkiewicz/leafbox-api)**.
- Based on the received data and predefined thresholds, the system can automatically activate pumps or solenoid valves to water the plants. 
- The configuration and control settings for each plant are managed through the **[Leafbox Web App](https://github.com/bSienkiewicz/leafbox)**, providing a seamless interface for users to interact with their plant monitoring system.

### Setup and Configuration

1. **Power Supply**: Ensure a stable 12V power supply to the system. It is later converted to 5V current with a step-down module.
2. **Sensors and Relays**: Connect up to 4 soil moisture sensors and 4 12V pumps or solenoid valves via the relay module.
3. **Firmware Installation**: Flash the ESP32 with the firmware provided in this repository.
4. **Network Setup**: Use the configuration webpage to connect the device to your Wi-Fi network and MQTT broker.
5. **Integration**: Use the **Leafbox Web App** to configure plant-specific settings and manage the watering schedule.

### Cost-Effective and Flexible

The **Leafbox ESP32** was designed to be a cost-effective yet powerful solution for automated plant care. Its flexibility allows users to adapt it to various types of plants and watering needs, making it an ideal choice for both small-scale hobbyists and larger plant management systems.
