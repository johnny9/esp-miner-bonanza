idf_component_register(
SRCS 
    "bm1370.c"
    "bm1368.c"
    "bm1366.c"
    "bm1397.c"
    "serial.c"
    "crc.c"
    "asic_common.c"
    "asic.c"
    "asic_driver.c"
    "asic_capabilities.c"
    "asic_job_store.c"
    "asic_result_handler.c"
    "bm_job_builder.c"
    "bm_result.c"
    "bzm.c"
    "bzm_reactor.c"
    "bzm_transport.c"
    "bzm_bridge.c"
    "bzm_power.c"
    "bzm_driver.c"
    "frequency_transition_bmXX.c"
    "pll.c"

INCLUDE_DIRS 
    "include"

REQUIRES 
    "freertos"
    "driver"
    "stratum"
    "stratum_v2"
    "tcp_transport"
    "esp_timer"
)


# Include the header files from "main" directory
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../main")

# Include the header files from "main/tasks" directory
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../main/tasks")
