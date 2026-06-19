#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <errno.h>
#include <netdb.h>            // struct addrinfo
#include <arpa/inet.h>
#include "esp_sntp.h"
#include "esp_netif_sntp.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "mdns.h"

#include "comms.h"
#include "networking.h"

#define TAG "comms"

static QueueHandle_t comms_queue;
static char payload_buf[1024];

int commsPost(struct comms_queue_item *message) {
  return xQueueSend(comms_queue, message, 0);
}


static int query_mdns_host(const char *host_name, struct in_addr *in_addr) {
	ESP_LOGI(TAG, "Query A: %s.local", host_name);

	struct esp_ip4_addr addr;
	addr.addr = 0;

	esp_err_t err = mdns_query_a(host_name, 2000, &addr);
	if(err){
		if(err == ESP_ERR_NOT_FOUND){
			ESP_LOGW(TAG, "%s: Host was not found!", host_name);
      return -118;
		} else {
			ESP_LOGE(TAG, "Query Failed: %s", esp_err_to_name(err));
      return -1;
		}
	}
	ESP_LOGI(TAG, "Query A: %s.local resolved to: " IPSTR, host_name, IP2STR(&addr));
  in_addr->s_addr = addr.addr;
  return 0;
}

void comms_thread(void *params) {
  ESP_LOGI(TAG, "INIT COMMS THREAD");
  int err;
  char append_buf[16];
  //Socket setup
  bool conn_ok;
  int sock = -1;
  //char host_ip[16];
  int addr_family = 0;
  int ip_protocol = 0;
  struct sockaddr_in dest_addr;
  dest_addr.sin_family = AF_INET;
  dest_addr.sin_port = htons(3133);
  addr_family = AF_INET;
  ip_protocol = IPPROTO_IP;

  struct comms_queue_item rmsg;
  //init queue
  comms_queue = xQueueCreate(
      100,
      sizeof(struct comms_queue_item)
      );
  //init networking 
  ESP_LOGI(TAG, "NETWORKING INIT");
  ESP_ERROR_CHECK(networking_init());
  //connect to network
  ESP_LOGI(TAG, "NETWORKING CONNECT");
  //NOTE: This function blocks until we get an IP address.
  esp_err_t ret = networking_connect(WIFI_SSID, WIFI_PASSWORD);
  if (ret != ESP_OK) {
      //TODO do some sort of retry logic here if the network is not reachable
      ESP_LOGE(TAG, "Failed to connect to Wi-Fi network");
  }
  wifi_ap_record_t ap_info;
  ESP_LOGI(TAG, "GET AP INFO");
  ret = esp_wifi_sta_get_ap_info(&ap_info);
  if (ret == ESP_ERR_WIFI_CONN) {
      ESP_LOGE(TAG, "Wi-Fi station interface not initialized");
  }
  else if (ret == ESP_ERR_WIFI_NOT_CONNECT) {
      ESP_LOGE(TAG, "Wi-Fi station is not connected");
  } else {
      ESP_LOGI(TAG, "--- Access Point Information ---");
      ESP_LOG_BUFFER_HEX("MAC Address", ap_info.bssid, sizeof(ap_info.bssid));
      ESP_LOG_BUFFER_CHAR("SSID", ap_info.ssid, sizeof(ap_info.ssid));
      ESP_LOGI(TAG, "Primary Channel: %d", ap_info.primary);
      ESP_LOGI(TAG, "RSSI: %d", ap_info.rssi);
  }
  //set up mDNS responder
  ESP_LOGD(TAG, "Setting up mDNS responder");
  err = mdns_init();
  if (err) {
    ESP_LOGE(TAG, "Failed to init mdns: %d", err);
  }
  err = mdns_hostname_set(MDNS_HOSTNAME);
  if (err) {
    ESP_LOGE(TAG, "Failed to set mdns hostname: %d", err);
  }
  err = mdns_instance_name_set(MDNS_INSTANCE_NAME);
  if (err) {
    ESP_LOGE(TAG, "Failed to set mdns instance name: %d", err);
  }
  //Set up SNTP
  esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
  esp_netif_sntp_init(&config);
  ESP_LOGI(TAG, "Waiting for SNTP response");
  if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(10000)) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to sync SNTP within 10 sec");
  } else {
    ESP_LOGI(TAG, "SNTP sync success! Current TS: %" PRId64 " ");
  }
  //TODO maybe release semaphore here to start sensor data collection
  //not actually required though, as the queue will discard stuff once it's full :)
  
  conn_ok=false;
  int64_t ts;
  int loops = 0;
  UBaseType_t items_waiting=-1;
  //NOTE: Ideally this would be in a state machine, but esp-idf doesn't have a proper state machine framework like zephyr does
  while (true) {
    //Try to connect to socket
    while (!conn_ok) {
      //Get the correct IP address
#ifdef MDNS_OVERRIDE
      //Note: some networks don't allow mDNS broadcasts.
      inet_aton(MDNS_OVERRIDE, &dest_addr.sin_addr);
#else

      ESP_LOGI(TAG, "QUERY MDNS section");
      err = -1;
      while (err!=0) {
        ESP_LOGI(TAG, "MDNS query fired");
        err = query_mdns_host("lwLIDAR_offload", &dest_addr.sin_addr);
        vTaskDelay(pdMS_TO_TICKS(1000));
      }
#endif
      if (sock!=-1) {
        ESP_LOGW(TAG, "Closing pre-exiting socket");
        shutdown(sock, SHUT_RDWR);
        vTaskDelay(pdMS_TO_TICKS(1000));

      }
      sock = socket(addr_family, SOCK_STREAM, ip_protocol);
      if (sock<0) {
        ESP_LOGE(TAG, "Unable to create socket, %d", sock);
        //TODO check if this returns 0 and connect returns 9
        //TODO and implement goto
        conn_ok = false;
        vTaskDelay(pdMS_TO_TICKS(1000));
        //NOTE: this would be an excellent spot for a GOTO statement
      }
      //Socket connect. in a loop so we can go back to retrying socket connection if it fails
      err = connect(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
      if (err != 0) {
        //Fail
        ESP_LOGE(TAG, "Socket unable to connect: errno %d", errno);
        //TODO maybe shut down socket here
        conn_ok = false;
        vTaskDelay(pdMS_TO_TICKS(1000));
      } else {
        //Success, exit loop
        ESP_LOGI(TAG, "Socket connect success!");
        conn_ok=true;
      }
    }
    //wait for queue message
    err =  xQueueReceive(
        comms_queue,
        &rmsg,
        pdMS_TO_TICKS(1000));
    //process queued message
    if (err == pdPASS) {
      switch (rmsg.type) {
        case COMMS_QUEUE_ITEM_TYPE_DISTANCES:
          //send message as actual TCP packetg
          snprintf(payload_buf, sizeof(payload_buf), "<> %d %" PRId64 " ", rmsg.distance_message.sensorIdx, (int64_t)rmsg.tv_now.tv_sec * 1000000L + (int64_t)rmsg.tv_now.tv_usec );
          for (int i=0; i<64; i++) {
            //Measurement valid?
            if (rmsg.distance_message.target_status[i] == 5 || rmsg.distance_message.target_status[i] == 9) {
              snprintf(append_buf, sizeof(append_buf), "%d ", rmsg.distance_message.distance_mm[i]);
            } else {
              snprintf(append_buf, sizeof(append_buf), "-%d ", rmsg.distance_message.target_status[i]);
            }
            //TODO check if payload_buf is actually big enough. If not, may get a BusFault crash or similar
            strncat(payload_buf, append_buf, sizeof(append_buf)-1);
          }
          strncat(payload_buf, "\n", 2);
          //Note: do NOT try to log data here, it will cause this thread to be a massive bottleneck
          //ESP_LOGI(TAG, "TCP Sensor %d", rmsg.distance_message.sensorIdx);
          //ESP_LOGI(TAG, "TCP: %s", payload_buf);
          err = send(sock, &payload_buf, strlen(payload_buf), 0);
          if (err<0) {
            //go back to reconnect tcp socket if it dies
            ESP_LOGE(TAG, "Error occurred during sending: errno %d", errno);
            conn_ok=false; 
          }
          break;
        case COMMS_QUEUE_ITEM_TYPE_STATS:
          //send message as actual TCP packet
          snprintf(payload_buf, sizeof(payload_buf), "<> S %" PRId64" %" PRId64" \n", (int64_t)rmsg.tv_now.tv_sec * 1000000L + (int64_t)rmsg.tv_now.tv_usec, rmsg.stats_message.loop_time);
          ESP_LOGI(TAG, "Loop time %" PRId64 "", rmsg.stats_message.loop_time);
          ESP_LOGI(TAG, "Sensor FPS %f", 1/(rmsg.stats_message.loop_time/1000000.f));
          err = send(sock, &payload_buf, strlen(payload_buf), 0);
          if (err<0) {
            //go back to reconnect tcp socket if it dies
            ESP_LOGE(TAG, "Error occurred during sending: errno %d", errno);
            conn_ok=false; 
          }
          break;
        //TODO optimise: store printf return to avoid strlen
        case COMMS_QUEUE_ITEM_TYPE_ACCEL:
          snprintf(payload_buf, sizeof(payload_buf), "<> A %" PRId64 " %f %f %f \n", (int64_t)rmsg.tv_now.tv_sec * 1000000L + (int64_t)rmsg.tv_now.tv_usec, rmsg.accel_message.accel_x, rmsg.accel_message.accel_y, rmsg.accel_message.accel_z);
          err = send(sock, &payload_buf, strlen(payload_buf), 0);
          if (err<0) {
            //go back to reconnect tcp socket if it dies
            ESP_LOGE(TAG, "Error occurred during sending: errno %d", errno);
            conn_ok=false;
          }
          break;
        case COMMS_QUEUE_ITEM_TYPE_GYRO:
          snprintf(payload_buf, sizeof(payload_buf), "<> G %" PRId64 " %f %f %f \n", (int64_t)rmsg.tv_now.tv_sec * 1000000L + (int64_t)rmsg.tv_now.tv_usec, rmsg.gyro_message.gyro_x, rmsg.gyro_message.gyro_y, rmsg.gyro_message.gyro_z);
          err = send(sock, &payload_buf, strlen(payload_buf), 0);
          if (err<0) {
            //go back to reconnect tcp socket if it dies
            ESP_LOGE(TAG, "Error occurred during sending: errno %d", errno);
            conn_ok=false;
          }
          break;
        case COMMS_QUEUE_ITEM_TYPE_MAG:
          snprintf(payload_buf, sizeof(payload_buf), "<> M %" PRId64 " %f %f %f \n", (int64_t)rmsg.tv_now.tv_sec * 1000000L + (int64_t)rmsg.tv_now.tv_usec, rmsg.mag_message.mag_x, rmsg.mag_message.mag_y, rmsg.mag_message.mag_z);
          err = send(sock, &payload_buf, strlen(payload_buf), 0);
          if (err<0) {
            //go back to reconnect tcp socket if it dies
            ESP_LOGE(TAG, "Error occurred during sending: errno %d", errno);
            conn_ok=false;
          }
          break;
        case COMMS_QUEUE_ITEM_TYPE_LSM_TEMP:
          snprintf(payload_buf, sizeof(payload_buf), "<> T %" PRId64 " %f \n",(int64_t)rmsg.tv_now.tv_sec * 1000000L + (int64_t)rmsg.tv_now.tv_usec, rmsg.lsm_temp_message.deg_c);
          err = send(sock, &payload_buf, strlen(payload_buf), 0);
          if (err<0) {
            //go back to reconnect tcp socket if it dies
            ESP_LOGE(TAG, "Error occurred during sending: errno %d", errno);
            conn_ok=false;
          }
          break;
      }
      loops++;
      if (loops%100==0) {
        items_waiting = uxQueueMessagesWaiting(comms_queue);
        ESP_LOGI(TAG, "IWAITING: %d", items_waiting);
      }
    }
  }
  //Unreachable
  ESP_ERROR_CHECK(networking_disconnect());

  ESP_ERROR_CHECK(networking_deinit());

  ESP_LOGI(TAG, "End of comms...");
}
