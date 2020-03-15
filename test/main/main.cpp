#include <stdlib.h>

#include <esp_event_loop.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <nvs_flash.h>

#include <Network/Clients/MQTT.hpp>


#define WIFI_SSID ""
#define WIFI_PASS ""

#define MQTT_HOST "mqtt.flespi.io"
#define MQTT_USER "try"
#define MQTT_PASS "try"

#define MQTT_PORT 1883
#define MQTTS_PORT 8883


#define LOGNAME "MQTT"

struct MessageReceiver : public Network::Client::MQTTv5::MessageReceived
{
    void messageReceived(const Network::Client::MQTTv5::DynamicStringView & topic, const Network::Client::MQTTv5::DynamicBinDataView & payload,
                         const uint16 packetIdentifier, const Network::Client::MQTTv5::PropertiesView & properties)
    {
        ESP_LOGI(LOGNAME, "Msg received: (%04X)", packetIdentifier);
        ESP_LOGI(LOGNAME, "  Topic: %.*s", topic.length, topic.data);
        ESP_LOGI(LOGNAME, "  Payload: %.*s", payload.length, payload.data);
    }
};

MessageReceiver receiver;
Network::Client::MQTTv5 client("eMQTT5", &receiver);

static void process(void *p) {
  for (;;) {
    if (Network::Client::MQTTv5::ErrorType ret = client.eventLoop())
    {
        ESP_LOGE(LOGNAME, "Event loop failed with error: %d", (int)ret);
        vTaskDelete(NULL);
        return;
    }
  }
}


static void connect() {
  ESP_LOGI(LOGNAME, "Starting MQTT");

  // initialize mqtt
  Network::Client::MQTTv5::DynamicBinDataView pw(strlen(MQTT_PASS), (const uint8*)MQTT_PASS);
  if (Network::Client::MQTTv5::ErrorType ret = client.connectTo(MQTT_HOST, MQTT_PORT, false, (uint16)30, true, MQTT_USER, strlen(MQTT_PASS) ? &pw : 0))
  {
      ESP_LOGE(LOGNAME, "Failed connection to %s with error: %d", MQTT_HOST, (int)ret);
      return;
  } 

  // publish packet first
  const char data[] = "{\"a\":3}";
  const char topic[] = "/testme";
  if (Network::Client::MQTTv5::ErrorType ret = client.publish(topic, (const uint8*)data, sizeof(data), false, Protocol::MQTT::V5::AtMostOne))
  {
      ESP_LOGE(LOGNAME, "Failed publishing %s to %s with error: %d", data, topic, (int)ret);
      return;
  }
  ESP_LOGI(LOGNAME, "Published %s to %s", data, topic);

  // subscribe to a topic
  if (Network::Client::MQTTv5::ErrorType ret = client.subscribe(topic, Protocol::MQTT::V5::GetRetainedMessageAtSubscriptionTime, true, Protocol::MQTT::V5::AtMostOne, false))
  {
      ESP_LOGE(LOGNAME, "Failed subscribing to %s with error: %d", topic, (int)ret);
      return;
  }
  ESP_LOGI(LOGNAME, "Subscribed to %s - Waiting for messages...", (const char*)topic);
  xTaskCreatePinnedToCore(process, "process", 2048, NULL, 10, NULL, 1);
}



static esp_err_t event_handler(void *ctx, system_event_t *event) {
  switch (event->event_id) {
    case SYSTEM_EVENT_STA_START:
      // connect to ap
      esp_wifi_connect();

      break;

    case SYSTEM_EVENT_STA_GOT_IP:
      // start mqtt
      connect();

      break;

    case SYSTEM_EVENT_STA_DISCONNECTED:
      // reconnect wifi
      esp_wifi_connect();

      break;

    default:
      break;
  }

  return ESP_OK;
}


extern "C" void app_main() {
  // initialize nvs flash
  ESP_ERROR_CHECK(nvs_flash_init());

  // initialize tcp/ip adapter
  tcpip_adapter_init();

  // register event handler
  ESP_ERROR_CHECK(esp_event_loop_init(event_handler, NULL));

  // initialize wifi
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  // set wifi storage to ram
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

  // set wifi mode
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

  // prepare wifi config
  wifi_config_t wifi_config = {};
  memcpy(wifi_config.sta.ssid, WIFI_SSID, strlen(WIFI_SSID));
  memcpy(wifi_config.sta.password, WIFI_PASS, strlen(WIFI_PASS));
  ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));

  // start wifi
  ESP_ERROR_CHECK(esp_wifi_start());

}
