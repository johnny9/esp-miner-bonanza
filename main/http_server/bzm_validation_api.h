#ifndef BZM_VALIDATION_API_H
#define BZM_VALIDATION_API_H

#include "esp_err.h"
#include "esp_http_server.h"

esp_err_t bzm_validation_api_register(httpd_handle_t server, void * user_context);

#endif /* BZM_VALIDATION_API_H */
