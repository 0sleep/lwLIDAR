set(srcs "lsm6dso_reg.c")
idf_component_register(SRCS ${srcs} 
  INCLUDE_DIRS "include"
  REQUIRES "driver esp_timer"
  )
