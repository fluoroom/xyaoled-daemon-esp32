#pragma once

#include "esp_err.h"

// Starts the HTTP server on the configured port, with /stream, and the
// port-80 redirect while the setup portal is open.
esp_err_t http_start(void);
