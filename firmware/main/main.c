#include <inttypes.h>

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "xyaoled";

void app_main(void)
{
	esp_chip_info_t chip;
	uint32_t flash_size = 0;

	esp_chip_info(&chip);
	esp_flash_get_size(NULL, &flash_size);

	ESP_LOGI(TAG, "XYAOLED firmware");
	ESP_LOGI(TAG, "cores %d, revision %d, flash %" PRIu32 " MB",
		 chip.cores, chip.revision, flash_size / (1024 * 1024));
	ESP_LOGI(TAG, "free heap %u bytes, largest block %u bytes",
		 (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
		 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}
