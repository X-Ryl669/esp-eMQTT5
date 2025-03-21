#include <stdlib.h>

#include <esp_event.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <nvs_flash.h>
#include "freertos/event_groups.h"

#include <Network/Clients/MQTT.hpp>


#define WIFI_SSID ""
#define WIFI_PASS ""

#define MQTT_HOST "mqtt.flespi.io"
#define MQTT_USER "try"
#define MQTT_PASS "try"

#define MQTT_PORT 1883
#define MQTTS_PORT 8883


#define LOGNAME "MQTT"

struct MessageReceiver : public Network::Client::MessageReceived
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
  if (Network::Client::MQTTv5::ErrorType ret = client.connectTo(MQTT_HOST, MQTTS_PORT, true, (uint16)30, true, MQTT_USER, strlen(MQTT_PASS) ? &pw : 0))
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
  // If you don't run the eventLoop in a task, because you only need to publish once, you'll Need to run the event loop for some time so the publish cycle can happen
  // uint32 publishCycleCount = (uint32)QoS;
  // while (publishCycleCount--)
  // {   // By a good design or pure randomness, the number of loop to run is equal to the QoS level...
  //     if (Network::Client::MQTTv5::ErrorType ret = client.eventLoop())
  //     {
  //         ESP_LOGE(LOGNAME, "Event loop failed with error: %d\n", (int)ret);
  //         return;
  //     }
  // }


  // subscribe to a topic
  if (Network::Client::MQTTv5::ErrorType ret = client.subscribe(topic, Protocol::MQTT::V5::GetRetainedMessageAtSubscriptionTime, true, Protocol::MQTT::V5::AtMostOne, false))
  {
      ESP_LOGE(LOGNAME, "Failed subscribing to %s with error: %d", topic, (int)ret);
      return;
  }
  ESP_LOGI(LOGNAME, "Subscribed to %s - Waiting for messages...", (const char*)topic);
  xTaskCreatePinnedToCore(process, "process", 2048, NULL, 10, NULL, 1);
}

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static EventGroupHandle_t s_wifi_event_group;
static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        ESP_LOGI("eMQTT5", "station starting");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
        ESP_LOGI("eMQTT5", "disconnected");
        xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI("eMQTT5", "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}


extern "C" void app_main() {
  s_wifi_event_group = xEventGroupCreate();

  // initialize nvs flash
  ESP_ERROR_CHECK(nvs_flash_init());

  // initialize tcp/ip adapter
  ESP_ERROR_CHECK(esp_netif_init());

  // register event handler
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_sta();

  esp_event_handler_instance_t instance_any_id;
  esp_event_handler_instance_t instance_got_ip;
  ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

  // initialize wifi
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  // set wifi storage to ram
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

  // set wifi mode
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

  // prepare wifi config
  wifi_config_t wifi_config = {};
  memset(&wifi_config, 0, sizeof(wifi_config));
  memcpy(wifi_config.sta.ssid, WIFI_SSID, strlen(WIFI_SSID));
  memcpy(wifi_config.sta.password, WIFI_PASS, strlen(WIFI_PASS));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

  // start wifi
  ESP_ERROR_CHECK(esp_wifi_start());

  // Can't call connect in event loop anymore since the event are called in sys task and it's too limited, so let's process them here
  EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE,
        pdFALSE,
        portMAX_DELAY);

  // xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually happened
  if (bits & WIFI_CONNECTED_BIT) {
      connect();
  }
}
