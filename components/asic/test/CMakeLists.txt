file(GLOB ASIC_TEST_SOURCES "*.c")
list(APPEND ASIC_TEST_SOURCES
     "../../../main/http_server/bzm_validation_api_contract.c"
     "../../../main/http_server/bzm_ota_guard.c")

idf_component_register(SRCS ${ASIC_TEST_SOURCES}
                       INCLUDE_DIRS "." "../../../main"
                                    "../../../main/http_server"
                       REQUIRES cmock stratum stratum_v2 asic json)
