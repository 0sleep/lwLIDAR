#ifndef _COMMS_H
#define _COMMS_H

#include "freertos/FreeRTOS.h"
#include "esp_sntp.h"

#define WIFI_SSID "CHANGEME_SSID"
#define WIFI_PASSWORD "CHANGEME_PASSWORD"
// Sometimes, we need to override mDNS due to network permissions limitations. 
// If this is required, uncomment this line and enter the host computer's IP
//#define MDNS_OVERRIDE "192.168.0.1"

#define MDNS_HOSTNAME "lwlidar"
#define MDNS_INSTANCE_NAME "lwLIDAR"

enum comms_queue_item_type {
  ///Distances from LIDAR Array
  COMMS_QUEUE_ITEM_TYPE_DISTANCES,
  //Accelerometer
  COMMS_QUEUE_ITEM_TYPE_ACCEL,
  //Gyroscope
  COMMS_QUEUE_ITEM_TYPE_GYRO,
  //MAgnetometer
  COMMS_QUEUE_ITEM_TYPE_MAG,
  COMMS_QUEUE_ITEM_TYPE_STATS,
  //Accelerometer die temperature
  COMMS_QUEUE_ITEM_TYPE_LSM_TEMP,
};

struct distance_q_msg {
  int sensorIdx;
  int16_t distance_mm[64];
  uint8_t target_status[64];
};

struct stats_q_msg {
  int64_t loop_time;
};

struct accel_q_msg {
  float accel_x;
  float accel_y;
  float accel_z;
};

struct gyro_q_msg {
  float gyro_x;
  float gyro_y;
  float gyro_z;
};

struct mag_q_msg {
  float mag_x;
  float mag_y;
  float mag_z;
};

struct lsm_temp_q_msg {
  float deg_c;
};

struct comms_queue_item {
  enum comms_queue_item_type type;
  struct timeval tv_now;
  union {
    struct distance_q_msg distance_message;
    struct stats_q_msg stats_message;
    struct accel_q_msg accel_message;
    struct gyro_q_msg gyro_message;
    struct mag_q_msg mag_message;
    struct lsm_temp_q_msg lsm_temp_message;
  };
};

int commsPost(struct comms_queue_item *message);

void comms_thread(void *params);

#endif //_COMMS_H
