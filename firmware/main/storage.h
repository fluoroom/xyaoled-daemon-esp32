// LittleFS on the scratch partition, mounted at /scratch: frames and GIFs
// being processed. Emptied at every boot; nothing in it outlives a send.
#pragma once

#include <stddef.h>

#include "esp_err.h"

#define SCRATCH "/scratch"

esp_err_t storage_init(void);
// Removes every file in /scratch.
void storage_clean(void);
void storage_usage(size_t *total, size_t *used);
size_t storage_free(void);
