#pragma once

#include "esp_http_server.h"

#define STREAM_MAX_FPS 5

esp_err_t stream_register(httpd_handle_t hd);
// Called for every closed socket; drops it if it was a stream client.
void stream_on_close(int fd);
