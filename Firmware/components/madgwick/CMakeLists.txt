set(srcs "MadgwickAHRS.c")
idf_component_register(SRCS ${srcs} 
  INCLUDE_DIRS "include"
  REQUIRES "driver esp_timer"
  )
