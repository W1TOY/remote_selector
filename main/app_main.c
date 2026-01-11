#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "protocol_examples_common.h"

#include "esp_log.h"
#include "mqtt_client.h"

#include "esp_wifi.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"
#include "lwip/sys.h"
#include "lwip/err.h"

#include "esp_log.h"
#include "mqtt_client.h"
#include "driver/gpio.h"

#include "driver/pulse_cnt.h"

#define GPIO_PIN_RELAY 23

static const char *TAG = "remote_selector_5";
#include "wifipassword.h"   // define following two variables in the wifipassword.h
//#define MY_ESP_WIFI_SSID      "mywifi_ssid"
//#define MY_ESP_WIFI_PASS      "password_for_mywifi"

#define MY_ESP_MAXIMUM_RETRY  5

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static int s_retry_num = 0;
int Antenna = -1;

//*************************************************************************** 
// Function to intialize the tuning board
//*************************************************************************** 
void setup_gpio_input(int pinnum)
{
    gpio_reset_pin((gpio_num_t)pinnum);
    gpio_intr_disable((gpio_num_t)pinnum);
    gpio_input_enable((gpio_num_t)pinnum);
}
void setup_gpio_output(int pinnum)
{
  gpio_reset_pin((gpio_num_t)pinnum);
  gpio_set_drive_capability((gpio_num_t)pinnum, GPIO_DRIVE_CAP_DEFAULT);
  gpio_set_direction((gpio_num_t)pinnum, GPIO_MODE_OUTPUT);
}
//***************************************************************************** 
// Wifi setup and reconnect
//***************************************************************************** 
static void log_error_if_nonzero(const char *message, int error_code)
{
    if (error_code != 0) {
        ESP_LOGE(TAG, "Last error %s: 0x%x", message, error_code);
    }
}

static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < MY_ESP_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG,"connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}
void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = MY_ESP_WIFI_SSID,
            .password = MY_ESP_WIFI_PASS,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start() );

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap SSID:%s password:%s",
                 MY_ESP_WIFI_SSID, MY_ESP_WIFI_PASS);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s",
                 MY_ESP_WIFI_SSID, MY_ESP_WIFI_PASS);
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }
}

//***************************************************************************** 
// MQTT handler
//***************************************************************************** 

esp_mqtt_client_handle_t client = NULL;

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    int tune_result = 0;
    esp_mqtt_event_handle_t event = event_data;
    client = event->client;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");

        esp_mqtt_client_subscribe(client, "remote_selector_5/A", 0);
        esp_mqtt_client_subscribe(client, "remote_selector_5/B", 0);
        esp_mqtt_client_subscribe(client, "remote_selector_5/selector_button", 0);

        esp_mqtt_client_publish(client, "remote_selector_5/A", "on", 0, 0, 0);
        esp_mqtt_client_publish(client, "remote_selector_5/B", "off", 0, 0, 0);
        esp_mqtt_client_publish(client,   "remote_selector_5/selected_Beacon", "off", 0, 0, 0);

        break;

    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT_EVENT_DATA");
        printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
        printf("DATA=%.*s\r\n", event->data_len, event->data);

        if (event->topic_len < 1) break;

        if ( strncmp(event->topic, "remote_selector_5/A", strlen("remote_selector_5/A")) == 0) {
            if ((strncmp(event->data, "on", 2) == 0) && Antenna ==0)
            {
                ESP_LOGI(TAG, "Antenna A is already selected");
//                esp_mqtt_client_publish(client, "remote_selector_5/B", "off", 0, 0, 0);
            }
            else if ((strncmp(event->data, "on", 2) == 0) && Antenna ==1)
            {
                Antenna = 0;
                gpio_set_level(GPIO_PIN_RELAY, 0);
                esp_mqtt_client_publish(client, "remote_selector_5/B", "off", 0, 0, 0);
                ESP_LOGI(TAG, "A is currently selected  \n");
            }
        }
        else if ( strncmp(event->topic, "remote_selector_5/B", strlen("remote_selector_5/B")) == 0) {
            if ((strncmp(event->data, "on", 2) == 0) && Antenna ==1)
            {
                ESP_LOGI(TAG, "Antenna B is already selected");
//                esp_mqtt_client_publish(client, "remote_selector_5/B", "off", 0, 0, 0);
            }
            else if ((strncmp(event->data, "on", 2) == 0) && Antenna ==0)
            {
                Antenna = 1;
                gpio_set_level(GPIO_PIN_RELAY, 1);
                esp_mqtt_client_publish(client, "remote_selector_5/A", "off", 0, 0, 0);
                ESP_LOGI(TAG, "B is currently selected  \n");
            }
        }
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            log_error_if_nonzero("reported from esp-tls", event->error_handle->esp_tls_last_esp_err);
            log_error_if_nonzero("reported from tls stack", event->error_handle->esp_tls_stack_err);
            log_error_if_nonzero("captured as transport's socket errno",  event->error_handle->esp_transport_sock_errno);
            ESP_LOGI(TAG, "Last errno string (%s)", strerror(event->error_handle->esp_transport_sock_errno));

        }
        break;

    case  MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DIsconnected");
        break;

    default:
        ESP_LOGI(TAG, "Other event id:%d", event->event_id);
        break;
    }
}

//***************************************************************************** 
// Start MQTT 
//***************************************************************************** 

static void mqtt_app_start(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = "mqtt://10.1.1.180",
        .credentials.username = "hamqtt",
        .credentials.authentication.password="931121",
        .network.disable_auto_reconnect = false,
        .network.reconnect_timeout_ms = 5000,
        .network.timeout_ms = 5000
    };

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    /* The last argument may be used to pass data to the event handler, in this example mqtt_event_handler */
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(client);
}


//***************************************************************************** 
// To send heart beat beacons to MQTT server and local LED to indicate that this system is operational
//***************************************************************************** 

int beacon_status = 0;
void vTaskPeriodic(void *pvParameters)
{
    for (;;) {
        if (client == NULL) continue;
        if (beacon_status == 0) {
            beacon_status = 1;
            esp_mqtt_client_publish(client, "remote_selector_5/selected_Beacon", "on", 0, 0, 0);

        }
        else {
            beacon_status = 0;
            esp_mqtt_client_publish(client, "remote_selector_5/selected_Beacon", "off", 0, 0, 0);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}


//***************************************************************************** 
// Main
//***************************************************************************** 


void app_main(void)
{
    static uint8_t ucParameterToPass;
    TaskHandle_t xHandle = NULL;
    

    ESP_LOGI(TAG, "[APP] Startup..");
    ESP_LOGI(TAG, "[APP] Free memory: %" PRIu32 " bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());


    setup_gpio_output(GPIO_PIN_RELAY);
    gpio_set_level(GPIO_PIN_RELAY, 0);
    Antenna = 0;
    nvs_flash_init();
    wifi_init_sta();
    mqtt_app_start();

    //*****************************************************
    // create tasks
    //*****************************************************

    xTaskCreate( vTaskPeriodic, "Periodic task", 4096, &ucParameterToPass, tskIDLE_PRIORITY, &xHandle );


}
