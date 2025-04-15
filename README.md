# esp-eMQTT5
A MQTT v5.0 client for ESP32 platform based on [eMQTT5](https://github.com/X-Ryl669/eMQTT5) library.

It's a fully compliant MQTT5 client to use in your projects.
Please refer to [library's documentation](https://blog.cyril.by/en/documentation/emqtt5-doc/emqtt5) for pros and cons of this client.

It's recommended to use version 2 of this library, available in branch v2.0.0 (or the master branch) since the version 1 is not more maintained.

For a benchmark of the library size, see below.

## Size difference and performance

At the time of writing, the only competitor for a MQTT v5 library on ESP32 was Wolfmqtt. Recently, Espressif added support for version 5 of the protocol in their MQTT client.

In order to be fair for comparing both libraries, the Espressif's version was configured with only MQTT V5 support (MQTT V3.1.1 was disabled). The example found in esp-idf repository was used.
It shows basic MQTT feature, such as using properties, publishing and subscribing and is equivalent to the example found in this library.

Meanwhile, this library gained optional features that aren't present in esp_mqtt, so a version with and without the optional feature is shown below

| Configuration | Total binary size (bytes) | Library size (bytes) |
|---------------|---------------------------|----------------------|
| esp_mqtt (v5) | 897792                    | 29936 (*)            |
| eMQTT5 opt    | 927680                    | 24712                |
| eMQTT5 min    | 825248                    | 17351                |

The `eMQTT5 opt` includes true QoS handling (with automatic packet saving and resending upon reconnect), Authentication, extended protocol validation and dumping, low latency support.
The `eMQTT5 min` is features-equivalent to esp_mqtt (v5)

`(*)` *The library size from esp_mqtt is computed by summing all file's size containing `mqtt` in their name from the `idf.py size-files` output. It's likely underestimating the actual library size since other source file aren't counted for.*

