#include  <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "thesis.h"
#include "comms.h"
#include "motion.h"

#define STACK_SIZE 15000


void app_main(void) {
  static uint8_t param;
  TaskHandle_t flexThreadHandle = NULL;
  TaskHandle_t motionThreadHandle = NULL;
  TaskHandle_t commsThreadHandle = NULL;
  //This task gets its' own core, hoping that it will run faster-er (yup)
  xTaskCreatePinnedToCore(thesis_flex_thread,
      "Flex Sensor Readout Thread",
      STACK_SIZE,
      &param,
      1, //HIGH Prio, maybe improves the 15ms gap  - it DOES! do not set it too high, the ESP will not start the USB stack, and then you're a bit screwed
      &flexThreadHandle,
      1 //Core 1
      );
  configASSERT(flexThreadHandle);
  xTaskCreatePinnedToCore(comms_thread,
      "Comms Thread",
      5000,
      &param,
      1, //Higher prio than motion thread
      &commsThreadHandle,
      0 //Core 0
      );
  configASSERT(commsThreadHandle);
  xTaskCreatePinnedToCore(motion_thread,
      "Motion Thread",
      5000,
      &param,
      0, //Low prio
      &motionThreadHandle,
      0 //Core 0
      );
  configASSERT(motionThreadHandle);
};
