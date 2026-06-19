#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "driver/spi_master.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/gpio.h>
#include "esp_log.h"
#include "lsm6dso_reg.h"

#include "motion.h"
#include "comms.h"


#define TAG "motion"


//TODO put in kconfig
#define PIN_NUM_MISO 13
#define PIN_NUM_MOSI 9
#define PIN_NUM_SCLK 12
#define PIN_NUM_LSM6_CS 10
#define PIN_NUM_LIS2_CS 47



// Gubbins for ST library integration
#define BOOT_TIME 100

static int16_t data_raw_acceleration[3];
static int16_t data_raw_angular_rate[3];
static int16_t data_raw_temperature;
static float_t acceleration_mg[3];
static float_t angular_rate_mdps[3];
static float_t temperature_degC;
static uint8_t whoamI, rst;
//static uint8_t tx_buffer[1000];

static int32_t platform_write(void *handle, uint8_t reg, const uint8_t *bufp,
                              uint16_t len);
static int32_t platform_read(void *handle, uint8_t reg, uint8_t *bufp,
                             uint16_t len);
static void platform_delay(uint32_t ms);
//end gubbins


void motion_thread(void *params) {
  ESP_LOGI(TAG, "Motion thread started");
  int err;
  //Set up SPI bus
  spi_device_handle_t lsm6handle;
  spi_device_handle_t lis2handle;
  spi_bus_config_t spi_bus_cfg = {
    .miso_io_num = PIN_NUM_MISO,
    .mosi_io_num = PIN_NUM_MOSI,
    .sclk_io_num = PIN_NUM_SCLK,
    .quadwp_io_num = -1,
    .quadhd_io_num = -1,
  };
  spi_device_interface_config_t lsm6cfg = {
    .clock_speed_hz = 100000,
    .mode = 3,
    .spics_io_num = PIN_NUM_LSM6_CS,
    .queue_size = 5,
    .address_bits=0,
  };
  //TODO start using this. Even though not implemented
  //add to SPI bus so it doesn't accidentally talk
  //due to floating CS pin
  spi_device_interface_config_t lis2cfg = {
    .flags = SPI_DEVICE_POSITIVE_CS,
    .clock_speed_hz = 100000,
    .mode = 0,
    .spics_io_num = PIN_NUM_LIS2_CS,
    .queue_size = 5,
  };
  err = spi_bus_initialize(SPI2_HOST, &spi_bus_cfg, SPI_DMA_CH_AUTO);
  ESP_ERROR_CHECK(err);
  err = spi_bus_add_device(SPI2_HOST, &lsm6cfg, &lsm6handle);
  ESP_ERROR_CHECK(err);
  err = spi_bus_add_device(SPI2_HOST, &lis2cfg, &lis2handle);
  ESP_ERROR_CHECK(err);
  //INIT SENSOR
  stmdev_ctx_t dev_ctx;
  /* Initialize mems driver interface */
  dev_ctx.write_reg = platform_write;
  dev_ctx.read_reg = platform_read;
  dev_ctx.mdelay = platform_delay;
  // I reckon this should be a spi_device_handle_t
  dev_ctx.handle = lsm6handle;

  //SOME MANUAL SPI GUBBINS;
  //get sensor ID: read 1 byte from 0x0F
  spi_transaction_t t;
  char sendbuf[2] = {0};
  char recvbuf[30] = {0};

  sendbuf[0] = 0x0F | 0x80;
  sendbuf[1] = 0x00;

  memset(&t, 0, sizeof(t));
  t.length = 16; //2 byte
  t.rx_buffer = recvbuf;
  t.tx_buffer = sendbuf;
  err = spi_device_transmit(lsm6handle, &t);
  ESP_ERROR_CHECK(err);
  ESP_LOGI(TAG, "GOT 0x%X on 0xF", recvbuf[1]);


  /* Wait sensor boot time */
  platform_delay(BOOT_TIME);
  /* Check device ID */
  lsm6dso_device_id_get(&dev_ctx, &whoamI);

  if (whoamI != LSM6DSO_ID) {
    ESP_LOGE(TAG, "WHOAMI is NOT LSM6DSO_ID (=0x%X), it's 0x%X", LSM6DSO_ID, whoamI);
    return;
  }
  /* Restore default configuration */
  lsm6dso_reset_set(&dev_ctx, PROPERTY_ENABLE);

  do {
    lsm6dso_reset_get(&dev_ctx, &rst);
  } while (rst);

  /* Disable I3C interface */
  lsm6dso_i3c_disable_set(&dev_ctx, LSM6DSO_I3C_DISABLE);
  /* Enable Block Data Update */
  lsm6dso_block_data_update_set(&dev_ctx, PROPERTY_ENABLE);
  /* Set Output Data Rate */
  lsm6dso_xl_data_rate_set(&dev_ctx, LSM6DSO_XL_ODR_12Hz5);
  lsm6dso_gy_data_rate_set(&dev_ctx, LSM6DSO_GY_ODR_12Hz5);
  /* Set full scale */
  lsm6dso_xl_full_scale_set(&dev_ctx, LSM6DSO_2g);
  lsm6dso_gy_full_scale_set(&dev_ctx, LSM6DSO_2000dps);
  /* Configure filtering chain(No aux interface)
   * Accelerometer - LPF1 + LPF2 path
   */
  lsm6dso_xl_hp_path_on_out_set(&dev_ctx, LSM6DSO_LP_ODR_DIV_100);
  lsm6dso_xl_filter_lp2_set(&dev_ctx, PROPERTY_ENABLE);

  /* Read samples in polling mode (no int) */
  struct comms_queue_item accel_msg = {
    .type = COMMS_QUEUE_ITEM_TYPE_ACCEL,
  };
  struct comms_queue_item gyro_msg = {
    .type = COMMS_QUEUE_ITEM_TYPE_GYRO,
  };
  struct comms_queue_item mag_msg = {
    .type = COMMS_QUEUE_ITEM_TYPE_MAG,
  };
  struct comms_queue_item lsm_temp_msg = {
    .type = COMMS_QUEUE_ITEM_TYPE_LSM_TEMP,
  };
  TickType_t last_sample_tick;
  const TickType_t motion_sample_freq = pdMS_TO_TICKS(100); //100hz possible?? Not without flag data ready get retry stuff. MAYBE ALSO USE INTERRUPTS FOR THIS?
  while (1) {
    //Lovely, the loop now runs at a set frequency.
    //Not so lovely: it only TRIES to get data at this frequency
    //which means that if a sensor isn't ready, we "skip" a sample
    //TODO MAYBE fix, if it's a big issue
    //FIX simpler: just try a few times lol (like LIDAR module)
    //FIX bestest: use the interrupts with a DRDY semaphore
    vTaskDelayUntil(&last_sample_tick, motion_sample_freq);

    uint8_t reg;
    /* Read output only if new xl value is available */
    lsm6dso_xl_flag_data_ready_get(&dev_ctx, &reg);

    if (reg) {
      gettimeofday(&accel_msg.tv_now, NULL);
      /* Read acceleration field data */
      memset(data_raw_acceleration, 0x00, 3 * sizeof(int16_t));
      lsm6dso_acceleration_raw_get(&dev_ctx, data_raw_acceleration);
      accel_msg.accel_message.accel_x = lsm6dso_from_fs2_to_mg(data_raw_acceleration[0]);
      accel_msg.accel_message.accel_y = lsm6dso_from_fs2_to_mg(data_raw_acceleration[1]);
      accel_msg.accel_message.accel_z = lsm6dso_from_fs2_to_mg(data_raw_acceleration[2]);
      commsPost(&accel_msg);

      /*
      acceleration_mg[0] =
        lsm6dso_from_fs2_to_mg(data_raw_acceleration[0]);
      acceleration_mg[1] =
        lsm6dso_from_fs2_to_mg(data_raw_acceleration[1]);
      acceleration_mg[2] =
        lsm6dso_from_fs2_to_mg(data_raw_acceleration[2]);
      ESP_LOGI(TAG, "Acceleration [mg]:%4.2f\t%4.2f\t%4.2f\r\n",
          acceleration_mg[0], acceleration_mg[1], acceleration_mg[2]);
          */

    }

    lsm6dso_gy_flag_data_ready_get(&dev_ctx, &reg);

    if (reg) {
      gettimeofday(&gyro_msg.tv_now, NULL);
      /* Read angular rate field data */
      memset(data_raw_angular_rate, 0x00, 3 * sizeof(int16_t));
      lsm6dso_angular_rate_raw_get(&dev_ctx, data_raw_angular_rate);
      gyro_msg.gyro_message.gyro_x = lsm6dso_from_fs2000_to_mdps(data_raw_angular_rate[0]);
      gyro_msg.gyro_message.gyro_y = lsm6dso_from_fs2000_to_mdps(data_raw_angular_rate[1]);
      gyro_msg.gyro_message.gyro_z = lsm6dso_from_fs2000_to_mdps(data_raw_angular_rate[2]);
      commsPost(&gyro_msg);
      /*
      angular_rate_mdps[0] =
        lsm6dso_from_fs2000_to_mdps(data_raw_angular_rate[0]);
      angular_rate_mdps[1] =
        lsm6dso_from_fs2000_to_mdps(data_raw_angular_rate[1]);
      angular_rate_mdps[2] =
        lsm6dso_from_fs2000_to_mdps(data_raw_angular_rate[2]);
      ESP_LOGI(TAG, "Angular rate [mdps]:%4.2f\t%4.2f\t%4.2f\r\n",
          angular_rate_mdps[0], angular_rate_mdps[1], angular_rate_mdps[2]);
          */
    }

    //NOTE: this is not really useful for the actual science (probably)
    //BUT it is very very useful for getting an idea how how the PCB
    //could be getting inside the case. (safety innit)
    lsm6dso_temp_flag_data_ready_get(&dev_ctx, &reg);

    if (reg) {
      gettimeofday(&lsm_temp_msg.tv_now, NULL);
      /* Read temperature data */
      memset(&data_raw_temperature, 0x00, sizeof(int16_t));
      lsm6dso_temperature_raw_get(&dev_ctx, &data_raw_temperature);
      lsm_temp_msg.lsm_temp_message.deg_c = lsm6dso_from_lsb_to_celsius(data_raw_temperature);
      commsPost(&lsm_temp_msg);
      /*
      temperature_degC =
        lsm6dso_from_lsb_to_celsius(data_raw_temperature);
      ESP_LOGI(TAG, "Temperature [degC]:%6.2f\r\n", temperature_degC);
      */
    }
  }
}

//Platform implementation loosely based on https://esp32.com/viewtopic.php?t=16389
static int32_t platform_read(void *handle, uint8_t reg, uint8_t *bufp, uint16_t len) {
  spi_transaction_t t;
  memset(&t, 0, sizeof(t));
  uint8_t tx[len+1];
  uint8_t rx[len+1];
  tx[0] = reg | 0x80;
  memset(&tx[1], 0x00, len);
  t.length = (len+1)*8;
  t.rx_buffer = rx;
  t.tx_buffer = tx;
  esp_err_t err = spi_device_transmit(handle, &t);
  ESP_ERROR_CHECK(err);
  memcpy(bufp, &rx[1], len);
  return 0;
}

static int32_t platform_write(void *handle, uint8_t reg, const uint8_t *bufp, uint16_t len) {
  spi_transaction_t t;
  memset(&t, 0, sizeof(t));
  uint8_t tx[len+1];
  tx[0] = reg & 0x7F;
  memcpy(&tx[1], bufp, len);
  t.length = (len+1)*8;
  t.tx_buffer = tx;
  t.rx_buffer = NULL;
  esp_err_t err = spi_device_transmit(handle, &t);
  ESP_ERROR_CHECK(err);
  return 0;
}

static void platform_delay(uint32_t ms) {
  vTaskDelay(pdMS_TO_TICKS(ms));
};

