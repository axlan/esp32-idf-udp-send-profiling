#include <stdio.h>
#include <string.h>

#include "driver/gptimer.h"
#include "esp_chip_info.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "freertos/ringbuf.h"
#include "esp_timer.h"

#include "lwip/udp.h"
#include "lwip/pbuf.h"
#include "lwip/api.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>

#include "mabutrace.h"

#include "secret.h"

#include "min_logger/min_logger.h"

// ESP-IDF Logging Tag
static const char *TAG = "UDPTest";

// Listen on host with:
// nc -kluvw 1 192.168.1.111 3333
#define HOST_IP_ADDR "192.168.1.192"
#define PORT 3333

// This accounts for the standard 1500 byte Maximum Transmission Unit (MTU)
// minus the 20-byte IP header and 8-byte UDP header (1500 - 20 - 8 = 1472).
static constexpr size_t UDP_MESSAGE_SIZE = 128;

// Semaphore to signal Wi-Fi connection
static SemaphoreHandle_t wifi_connected_sem;

// Data logging ring buffer
static RingbufHandle_t buf_handle;

extern "C"
{
  size_t min_logger_get_thread_name(char *thread_name, size_t max_len)
  {
    char *taskName = pcTaskGetName(NULL);
    strncpy(thread_name, taskName, max_len);
    thread_name[max_len - 1] = 0;
    return strlen(thread_name);
  }

  uint64_t min_logger_get_time_nanoseconds()
  {
    return esp_timer_get_time() * 1000;
  }

  void min_logger_write(const uint8_t *msg, size_t len_bytes)
  {
    UBaseType_t res = xRingbufferSend(buf_handle, msg, len_bytes, 0);
    if (res != pdTRUE)
    {
      ESP_LOGE(TAG, "Failed to send item");
    }
  }
}

// Wi-Fi event handler
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
  {
    esp_wifi_connect();
  }
  else if (event_base == WIFI_EVENT &&
           event_id == WIFI_EVENT_STA_DISCONNECTED)
  {
    ESP_LOGI(TAG, "Retrying connection to the AP");
    esp_wifi_connect();
  }
  else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
  {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "Got IP:" IPSTR, IP2STR(&event->ip_info.ip));
    xSemaphoreGive(wifi_connected_sem);
  }
}

void wifi_init_sta(void)
{
  wifi_connected_sem = xSemaphoreCreateBinary();

  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_sta();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  esp_event_handler_instance_t instance_any_id;
  esp_event_handler_instance_t instance_got_ip;
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL,
      &instance_any_id));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL,
      &instance_got_ip));

  wifi_config_t wifi_config = {};
  strcpy((char *)wifi_config.sta.ssid, ssid);
  strcpy((char *)wifi_config.sta.password, password);
  wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
  ESP_ERROR_CHECK(esp_wifi_start());

  ESP_LOGI(TAG, "wifi_init_sta finished.");
  printf("Connecting to %s ", ssid);

  // Wait for connection
  xSemaphoreTake(wifi_connected_sem, portMAX_DELAY);
  printf("\nWiFi connected.\n");
}

static void log_heap()
{
  size_t cur_heap = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
  size_t min_heap = heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT);
  ESP_LOGI(TAG, "min_heap/cur_heap %zu/%zu", min_heap, cur_heap);
}

// https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/lwip.html
// https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/system/freertos_additions.html#ring-buffers
// Basic theory is to use a ring buffer to sync multiple tasks writing to buffer that is drained by this UDP client task.
// To mimimize memory use and not block the write tasks, a ring buffer is created with 2x the UDP send size (500 byte Maximum Transmission Unit (MTU) minus the 20-byte IP header and 8-byte UDP header (1500 - 20 - 8 = 1472))
// This effectively lets the buffer work like a ring buffer for writes, and a ping-pong buffer for reads.
// Using the IDF FreeRTOS RingBuffer extension almost is able to work ideally. The one limitation is that it can't block until a minimum amount of data is available. I could write my own data structure more tailored for this use case, but for now I'll just poll the buffer in this task.

// https://www.nongnu.org/lwip/2_0_x/group__pbuf.html
// https://www.nongnu.org/lwip/2_0_x/group__udp__raw.html
// This is supposedly not supported by the ESP-IDF framework https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/lwip.html, but it is what the Arduino core uses for AsyncUDP https://github.com/espressif/arduino-esp32/blob/master/libraries/AsyncUDP/src/AsyncUDP.cpp.
static void udp_client_task_raw(void *pvParameters)
{
  struct udp_pcb *pcb;
  pcb = udp_new();
  udp_bind(pcb, IP_ADDR_ANY, 0);

  ip_addr_t dest_ip;
  dest_ip.type = IPADDR_TYPE_V4;
  dest_ip.u_addr.ip4.addr = inet_addr(HOST_IP_ADDR);

  // Allocate a pbuf that will point to a block of read only memory. In this case it will point to a half of the ring buffer being held.
  struct pbuf *pbuf = pbuf_alloc(PBUF_TRANSPORT, UDP_MESSAGE_SIZE, PBUF_ROM);
  assert(pbuf != NULL);

  // This is effectively const, but needs to be mutable to match pbuf typing since it's used for recieve as well as send.
  void *held_data = NULL;

  while (1)
  {
    {
      TRACE_SCOPE("udp_update");
      // Check if UDP send is done. If so return data to ring buffer.
      if (held_data != NULL && pbuf->ref == 1)
      {
        vRingbufferReturnItem(buf_handle, held_data);
        TRACE_INSTANT("free");
        held_data = NULL;
      }

      // Check if a UDP packet's worth of data is ready to send.
      if (xRingbufferGetCurFreeSize(buf_handle) <= UDP_MESSAGE_SIZE)
      {
        TRACE_SCOPE("udp_send");
        size_t read_size;
        // By always reading half the buffer size, the read will never be limitted by rolling over the end of the buffer.
        held_data = xRingbufferReceiveUpTo(buf_handle, &read_size, pdMS_TO_TICKS(portMAX_DELAY), UDP_MESSAGE_SIZE);
        assert(read_size == UDP_MESSAGE_SIZE);
        pbuf->payload = held_data;
        pbuf->tot_len = UDP_MESSAGE_SIZE;
        udp_sendto(pcb, pbuf, &dest_ip, PORT);
        log_heap();
      }
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

// https://www.nongnu.org/lwip/2_0_x/api_8h.html
// https://lwip.fandom.com/wiki/Netconn_API
static void udp_client_task_netconn(void *pvParameters)
{
  struct netconn *conn;
  struct netbuf *buf;
  err_t err;

  ip_addr_t dest_ip;
  dest_ip.type = IPADDR_TYPE_V4;
  dest_ip.u_addr.ip4.addr = inet_addr(HOST_IP_ADDR);

  // Create a new connection identifier for UDP
  conn = netconn_new(NETCONN_UDP);
  assert(conn != NULL);

  buf = netbuf_new();

  // This is effectively const, but needs to be mutable to match pbuf typing since it's used for recieve as well as send.
  void *held_data = NULL;

  while (1)
  {
    {
      TRACE_SCOPE("udp_update");
      // Assume UDP send is done and return data to ring buffer.
      if (held_data != NULL)
      {
        vRingbufferReturnItem(buf_handle, held_data);
        TRACE_INSTANT("free");
        held_data = NULL;
      }

      // Check if a UDP packet's worth of data is ready to send.
      if (xRingbufferGetCurFreeSize(buf_handle) <= UDP_MESSAGE_SIZE)
      {
        TRACE_SCOPE("udp_send");
        size_t read_size;
        // By always reading half the buffer size, the read will never be limitted by rolling over the end of the buffer.
        held_data = xRingbufferReceiveUpTo(buf_handle, &read_size, pdMS_TO_TICKS(portMAX_DELAY), UDP_MESSAGE_SIZE);
        assert(read_size == UDP_MESSAGE_SIZE);
        err = netbuf_ref(buf, held_data, UDP_MESSAGE_SIZE);
        assert(err == ERR_OK);

        err = netconn_sendto(conn, buf, &dest_ip, PORT);
        assert(err == ERR_OK);

        log_heap();
      }
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

// https://github.com/espressif/esp-idf/tree/master/examples/protocols/sockets/udp_client
static void udp_client_task_bsd(void *pvParameters)
{
  int addr_family = AF_INET;
  int ip_protocol = IPPROTO_IP;

  struct sockaddr_in dest_addr;
  dest_addr.sin_addr.s_addr = inet_addr(HOST_IP_ADDR);
  dest_addr.sin_family = AF_INET;
  dest_addr.sin_port = htons(PORT);

  int sock = socket(addr_family, SOCK_DGRAM, ip_protocol);
  if (sock < 0)
  {
    ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
    return;
  }

  // Set timeout
  struct timeval timeout;
  timeout.tv_sec = 0;
  timeout.tv_usec = 10 * 1000;
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

  while (1)
  {
    {
      TRACE_SCOPE("udp_update");

      // Check if a UDP packet's worth of data is ready to send.
      if (xRingbufferGetCurFreeSize(buf_handle) <= UDP_MESSAGE_SIZE)
      {
        TRACE_SCOPE("udp_send");
        size_t read_size;
        // By always reading half the buffer size, the read will never be limitted by rolling over the end of the buffer.
        void *held_data = xRingbufferReceiveUpTo(buf_handle, &read_size, pdMS_TO_TICKS(portMAX_DELAY), UDP_MESSAGE_SIZE);
        assert(read_size == UDP_MESSAGE_SIZE);
        int err = sendto(sock, held_data, UDP_MESSAGE_SIZE, 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
        // Don't need to hold buffer since it is always coppied by sendto.
        vRingbufferReturnItem(buf_handle, held_data);
        if (err < 0)
        {
          ESP_LOGE(TAG, "Error occurred during sending: errno %d", errno);
          break;
        }
        log_heap();
      }
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

static void data_gen_task(void *pvParameters)
{
  char *taskName = pcTaskGetName(NULL);
  while (true)
  {
    {
      TRACE_SCOPE("data_gen");
      MIN_LOGGER_RECORD_AND_LOG_VALUE_ARRAY(MIN_LOGGER_INFO, "data_gen", char, taskName, strlen(taskName), "${data_gen}");
    }
    vTaskDelay(50 / portTICK_PERIOD_MS);
  }
}

extern "C" void app_main(void)
{
  // Initialize NVS
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
  {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  buf_handle = xRingbufferCreate(UDP_MESSAGE_SIZE * 2, RINGBUF_TYPE_BYTEBUF);
  assert(buf_handle != NULL);

  // Initialize WiFi
  wifi_init_sta();

  // Setup as many worker tasks as there are cpu cores
  esp_chip_info_t chip_info;
  esp_chip_info(&chip_info);

  // Create tasks.
  xTaskCreate(data_gen_task, "data_gen_task 1", 4096, NULL, 5, NULL);
  xTaskCreate(data_gen_task, "data_gen_task 2", 4096, NULL, 5, NULL);
  // xTaskCreate(udp_client_task_netconn, "udp_client", 4096, NULL, 1, NULL);
  // xTaskCreate(udp_client_task_raw, "udp_client", 4096, NULL, 1, NULL);
  xTaskCreate(udp_client_task_bsd, "udp_client", 4096, NULL, 1, NULL);

  // Initialize MabuTrace and start server on port 81
  ESP_ERROR_CHECK(mabutrace_init());
  ESP_ERROR_CHECK(mabutrace_start_server(81));

  // Get IP Address
  esp_netif_ip_info_t ip_info;
  esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  esp_netif_get_ip_info(netif, &ip_info);

  printf("MabuTrace server started. Go to http://" IPSTR ":81/ to capture a trace.\n",
         IP2STR(&ip_info.ip));

  min_logger_write_thread_names();
  MIN_LOGGER_LOG(MIN_LOGGER_INFO, "Start");

  for (;;)
  {
    vTaskDelay(portMAX_DELAY);
  }
}
