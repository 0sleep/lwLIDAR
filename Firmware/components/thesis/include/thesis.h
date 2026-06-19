#ifndef _THESIS_H
#define _THESIS_H

#define MUX_I2C_SPEED_HZ 1000000
#define SENSOR_I2C_SPEED_HZ 1000000

#define MUX_ADDR 0x44
#define VL53_DEFAULT_ADDR 0x29
#define VL53_1_NEW_ADDR 0x10
#define VL53_2_NEW_ADDR 0x20
#define VL53_3_NEW_ADDR 0x30
#define VL53_4_NEW_ADDR 0x40
#define VL53_5_NEW_ADDR 0x50
#define VL53_6_NEW_ADDR 0x60
#define VL53_7_NEW_ADDR 0x15
#define VL53_8_NEW_ADDR 0x25

#define VL53_SYNC_PIN 48

#define INT_PIN 18

#define LED_PIN 17

//scan the i2c bus
//just for manual debug for now.
//maybe in the future I could scan the I2C bus and determine if 
//the sensors have been set to the correct addresses yet
int scan_i2c_bus();

//Handle for thread
void thesis_flex_thread(void *params);

#endif //_THESIS_H
