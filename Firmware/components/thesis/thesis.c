#include <stdlib.h>
#include <stdio.h>
#include "esp_timer.h"
#include "driver/gpio.h"
#include "esp_sntp.h"
#include "vl53l8cx_api.h"

#include "thesis.h"
#include "comms.h"

//I2C gubbins
static i2c_port_t i2c_port;
static i2c_master_bus_handle_t bus_handle;
static i2c_master_bus_config_t bus_config;

static i2c_master_dev_handle_t mux_handle;

static i2c_master_dev_handle_t sensor_handle[8];
static i2c_device_config_t sensor_config[8];
//Sensor gubbins
static VL53L8CX_Configuration vl_config[8];
static VL53L8CX_ResultsData vl_results[8] __attribute__((aligned(4)));

//RTOS sync stuff
SemaphoreHandle_t int_sem = NULL;
SemaphoreHandle_t fps_sem = NULL;

//TODO future just calculate these properly
uint8_t mux_lut[] = {0b00000001,0b00000010,0b00000100,0b00001000,0b00010000,0b00100000,0b01000000,0b10000000};

static int mux_init() {
  //TODO not sure if this could break things.
  //I am dynamically? allocating this struct,
  //but then making the mux_handle point to it
  //what happens when I exit this function?
  esp_err_t err;
  uint8_t output_cmd[] = {0x03, 0xff};
  uint8_t reg_follow_cmd[] = {0x07, 0x00};
  uint8_t low_cmd[] = {0x05, 0x00};
  //Set all pins to OUTPUT
  err = i2c_master_transmit(mux_handle, output_cmd, 2, 50);
  portYIELD();
  //Set all pins to follow register 0x05
  err = i2c_master_transmit(mux_handle, reg_follow_cmd, 2, 50);
  portYIELD();
  //Set all pins to LOW
  err = i2c_master_transmit(mux_handle, low_cmd, 2, 50);
  portYIELD();
  return 0;
}

//Send a bitmask of 8 bits to the sensor.
//Bits that are 1 will set the corresponding
//mux pin HIGH
static int mux_set(uint16_t pins) {
  esp_err_t err;
  uint8_t data[2] = {
    0x05,
    pins
  };
  printf("Mux set 0x%X\n", pins);
  err = i2c_master_transmit(mux_handle, data, 2, 50);
  return 0;
}

//To synchronise main loop with sensor 1
static void IRAM_ATTR int_isr(void *arg) {
  //We don't care about this val
  BaseType_t htpw = pdFALSE;
  xSemaphoreGiveFromISR(int_sem, &htpw);
}

//To slow down reading attempt speed to max 16 hz (currently at 21 hz, but unstable)
static void IRAM_ATTR fps_isr(void *arg) {
  //We don't care about this val
  BaseType_t htpw = pdFALSE;
  xSemaphoreGiveFromISR(fps_sem, &htpw);
}

static int set_and_move_sensors() {
  uint64_t start = esp_timer_get_time();
  uint8_t new_addr[] = {
    VL53_1_NEW_ADDR,
    VL53_2_NEW_ADDR,
    VL53_3_NEW_ADDR,
    VL53_4_NEW_ADDR,
    VL53_5_NEW_ADDR,
    VL53_6_NEW_ADDR,
    VL53_7_NEW_ADDR,
    VL53_8_NEW_ADDR
  };
  uint8_t i2c_address[2];
  uint8_t buf[1];
  i2c_master_transmit_multi_buffer_info_t i2c_buffers[2];
  esp_err_t err;
  for (int i=0; i<8; i++) {
    portYIELD();
    sensor_config[i].dev_addr_length = I2C_ADDR_BIT_LEN_7;
    sensor_config[i].device_address = VL53_DEFAULT_ADDR;
    sensor_config[i].scl_speed_hz = SENSOR_I2C_SPEED_HZ;
    printf("Moving sensor %d\n", i);
    err = i2c_master_bus_add_device(bus_handle, &sensor_config[i], &sensor_handle[i]);
    //send 0x01, 0x02, 0x04, 0x08 ...
    //mux_set((1<<(i+1))-1);
    
    mux_set(mux_lut[i]);

    //register 0x7fff write 0x00
    printf("0x7fff write 0x00\n");
    i2c_address[0] = 0x7f;
    i2c_address[1] = 0xff;
    i2c_buffers[0].write_buffer = i2c_address;
    i2c_buffers[0].buffer_size = 2;

    buf[0] = 0x00;
    i2c_buffers[1].write_buffer = buf;
    i2c_buffers[1].buffer_size = 1;

    i2c_master_multi_buffer_transmit(sensor_handle[i], i2c_buffers, 2, 50);

    //register 0x04 write new i2c adress (7 BIT ADDRESS, IGNORE ST's API!)
    printf("0x04 write 0x%X\n", new_addr[i]);
    i2c_address[0] = 0x00;
    i2c_address[1] = 0x04;
    buf[0] = new_addr[i];
    i2c_master_multi_buffer_transmit(sensor_handle[i], i2c_buffers, 2, 50);

    //update the device address
    // check if this is sufficient. if not, remove, reconfig, readd device
    //NOTE: no. kept for posterity
    //sensor_config[i].device_address = new_addr[i];
    i2c_master_bus_rm_device(sensor_handle[i]);
    sensor_config[i].device_address = new_addr[i];
    i2c_master_bus_wait_all_done(bus_handle, -1);

    i2c_master_bus_add_device(bus_handle, &sensor_config[i], &sensor_handle[i]);

    //register 0x7fff write 0x02
    printf("0x7fff write 0x02\n");
    i2c_address[0] = 0x7f;
    i2c_address[1] = 0xff;
    buf[0] = 0x02;
    i2c_master_multi_buffer_transmit(sensor_handle[i], i2c_buffers, 2, 50);
    i2c_master_bus_wait_all_done(bus_handle, -1);
  }
  uint64_t end = esp_timer_get_time();
  printf("Moving sensors took %llu ms\n", (end-start)/1000);
  return 0;
}

//init sensor (note that i2c_device_handle_t is ALREADY a pointer...
//thanks espressif, not confusing at all
//there's a reason why you're discouraged from using
//typedefs all over the place
static int init_sensor(i2c_master_dev_handle_t sensor_handle, int sensor_num) {
  uint8_t err;
  uint8_t alive;
  vl_config[sensor_num].platform.bus_config = bus_config;
  vl_config[sensor_num].platform.handle = sensor_handle;
  vl_config[sensor_num].platform.address = sensor_config[sensor_num].device_address;
  err = vl53l8cx_is_alive(&vl_config[sensor_num], &alive);
  if (!alive || err) {
    printf("VL53 is not alive :(\n");
    return -1;
  } else {
    printf("Sensor is alive!\n");
  }
  //TODO check firmware checksum. if checksum is fine, skip _init to save a lot of time
  err = vl53l8cx_init(&vl_config[sensor_num]);
  if (err) {
    printf("VL53 ULD Loading failed\n");
    return -1;
  } else {
    printf("VL53 ULD Load success!\n");
  }
	err = vl53l8cx_set_resolution(&vl_config[sensor_num], VL53L8CX_RESOLUTION_8X8);
	if(err)
	{
		printf("vl53l8cx_set_resolution failed, status %u\n", err);
		return -1;
	}
  //TODO probably obsolete due to SYNC pin
	err = vl53l8cx_set_ranging_frequency_hz(&vl_config[sensor_num], 20);
  if (err) {
		printf("vl53l8cx_set_ranging_frequency failed, status %u\n", err);
		return -1;
  }
  //Enable SYNC pin
  err = vl53l8cx_set_external_sync_pin_enable(&vl_config[sensor_num], 1);
  if (err) {
		printf("vl53l8cx_set_external_sync_pin_enable failed, status %u\n", err);
		return -1;

  }
  return 0;
}

static int init_all() {
  //Set up INT pin intterupt stuff
  int_sem = xSemaphoreCreateBinary();
  gpio_config_t int_conf = {
    .intr_type = GPIO_INTR_POSEDGE,
    .mode = GPIO_MODE_INPUT,
    .pin_bit_mask = (1ULL << INT_PIN),
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .pull_up_en = GPIO_PULLUP_DISABLE,
  };
  gpio_config(&int_conf);
  gpio_install_isr_service(0);
  gpio_isr_handler_add(INT_PIN, int_isr, NULL);
  //Set up SYNC pin
  gpio_reset_pin(VL53_SYNC_PIN);
  gpio_set_direction(VL53_SYNC_PIN, GPIO_MODE_OUTPUT);
  //Set up I2C
  i2c_port = I2C_NUM_1;
  bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
  bus_config.i2c_port = i2c_port;
  //TODO make Kconfig-able
  bus_config.scl_io_num = 4;
  bus_config.sda_io_num = 5;
  bus_config.glitch_ignore_cnt = 7;
  bus_config.flags.enable_internal_pullup = true;
  ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &bus_handle));
  //Set up mux
  i2c_device_config_t mux_config = {
    .dev_addr_length = I2C_ADDR_BIT_LEN_7,
    .device_address = MUX_ADDR,
    .scl_speed_hz = MUX_I2C_SPEED_HZ,
  };
  if (i2c_master_bus_add_device(bus_handle, &mux_config, &mux_handle) != ESP_OK) {
    printf("Failed to add mux to bus\n");
    return -1;
  }
  printf("Mux init\n");
  mux_init();
  //TODO BUS SCAN, SEE IF SENSORS ARE MOVED ALREADY
  //mux_set(0b11111111);
  //....
  
  //Move each sensor to its' "correct" position
  printf("Moving sensors\n");
  set_and_move_sensors();
  //Mux set all pins to ON. This should be fine as the sensors are now on different addresses
  mux_set(0b11111111);
  //"INIT" each sensor (load the firmware etc)
  uint64_t start = esp_timer_get_time();
  for (int i=0; i<8; i++) {
    printf("Initing sensor %d\n", i);
    init_sensor(sensor_handle[i], i);
    portYIELD();
  }
  uint64_t end = esp_timer_get_time();
  printf("Loading 8 ULD fw took %llu ms\n", (end-start)/1000);
  //scan_i2c_bus();
  return 0;
}

void thesis_flex_thread(void *params) {

  int err;
  uint64_t loopStartTime;
  uint64_t loopStopTime;
  uint8_t isReady;
  //To contain the previous loop's timestamp
  struct timeval prev_loop_tv;
  TickType_t last_wake;
  const TickType_t loop_max_frequency = pdMS_TO_TICKS(62); //~16 hz
  int tries = 0;
  //Queue-able data message
  struct comms_queue_item qmsg;
  qmsg.type = COMMS_QUEUE_ITEM_TYPE_DISTANCES;
  //Queue-able statistics message
  struct comms_queue_item smsg;
  smsg.type = COMMS_QUEUE_ITEM_TYPE_STATS;
  //LED SETUP
  gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);
  //LED ON
  gpio_set_level(LED_PIN, 1);
  //wait, to give me enough time between plugging in and opening monitor (or re-flashing)
  //as I need to hard-reset the sensors
  //TODO remove, obsolete
  printf("DELAY 10 sec\n");
  vTaskDelay(pdMS_TO_TICKS(100));
  printf("END DELAY\n");
  //LED OFF
  gpio_set_level(LED_PIN, 0);
  init_all();
  //TODO check if these are at all required
  vl53l8cx_start_ranging(&vl_config[0]);
  vl53l8cx_start_ranging(&vl_config[1]);
  vl53l8cx_start_ranging(&vl_config[2]);
  vl53l8cx_start_ranging(&vl_config[3]);
  vl53l8cx_start_ranging(&vl_config[4]);
  vl53l8cx_start_ranging(&vl_config[5]);
  vl53l8cx_start_ranging(&vl_config[6]);
  vl53l8cx_start_ranging(&vl_config[7]);
  //TODO maybe this is bad.
  vTaskDelay(pdMS_TO_TICKS(100));
  last_wake = xTaskGetTickCount();
  //LED ON
  gpio_set_level(LED_PIN, 1);
  while (true) {
    //Limit loop rate to max frequency
    vTaskDelayUntil(&last_wake, loop_max_frequency);
    //Flick SYNC pin
    gpio_set_level(VL53_SYNC_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(1));
    gpio_set_level(VL53_SYNC_PIN, 0);
    //Set timestamp. This will then be the same for every sensor in this "SYNC" cycle
    //Note that because we are being a little bit cheeky with when we read the sensor data,
    //the SYNC pulse that just happened is for NEXT loop iteration, so we temporarily store
    //the current timestamp in prev_loop to make it be correct :)
    qmsg.tv_now = prev_loop_tv;
    gettimeofday(&prev_loop_tv, NULL);
    //Wait for sensor 1 interrupt pin
    if (xSemaphoreTake(int_sem, portMAX_DELAY) == pdTRUE) {
      //Read all sensors. Assumption is that by the time we ha ve finished reading sensor 1, sensor 2 will be ready as well
      for (int snum=0; snum < 8; snum++) {
        isReady=false;
        tries = 0;
        while (!isReady && tries < 10) {
          tries++;
          vl53l8cx_check_data_ready(&vl_config[snum], &isReady);
          i2c_master_bus_wait_all_done(bus_handle, -1);
          if (isReady) {
            //Get ranging data
            err = vl53l8cx_get_ranging_data(&vl_config[snum], &vl_results[snum]);
            //Wait for i2c transaction to complete
            i2c_master_bus_wait_all_done(bus_handle, -1);
            //Populate sensor message
            qmsg.distance_message.sensorIdx = snum;
            memcpy(qmsg.distance_message.distance_mm, vl_results[snum].distance_mm, sizeof(qmsg.distance_message.distance_mm));
            memcpy(qmsg.distance_message.target_status, vl_results[snum].target_status, sizeof(qmsg.distance_message.target_status));
            //Send item to queue
            commsPost(&qmsg);
          }
        }
      }
    }
    //Wait a little bit before flicking the SYNC pin. It seems to kill the last sensor otherwise
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}
