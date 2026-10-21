// XYAOLED firmware: keeps a BLE connection to an XYAOLED LED panel and
// serves the HTTP API of the Go daemon (reference/go-daemon) over Wi-Fi.
#include <inttypes.h>

#include "display.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "http.h"
#include "nvs_flash.h"
#include "settings.h"
#include "storage.h"
#include "wifi.h"

static const char *TAG = "xyaoled";

static void log_heap(const char *when)
{
	ESP_LOGI(TAG, "%s: free heap %u bytes, largest block %u bytes", when,
		 (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
		 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

void app_main(void)
{
	esp_chip_info_t chip;
	uint32_t flash_size = 0;
	esp_chip_info(&chip);
	esp_flash_get_size(NULL, &flash_size);
	ESP_LOGI(TAG, "XYAOLED firmware");
	ESP_LOGI(TAG, "cores %d, revision %d, flash %" PRIu32 " MB", chip.cores, chip.revision,
		 flash_size / (1024 * 1024));
	log_heap("boot");

	esp_err_t err = nvs_flash_init();
	if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		ESP_ERROR_CHECK(nvs_flash_erase());
		err = nvs_flash_init();
	}
	ESP_ERROR_CHECK(err);
	ESP_ERROR_CHECK(settings_init());

	const settings_t *s = settings_get();
	ESP_LOGI(TAG, "Configuration:");
	ESP_LOGI(TAG, "  Panel: %ux%u, match %s %s", s->panel_width, s->panel_height,
		 s->panel_by_mac ? "MAC" : "name", s->panel_by_mac ? s->panel_mac : s->panel_name);
	ESP_LOGI(TAG, "  Upside down: %s, passthrough: %s", s->upside_down ? "yes" : "no",
		 s->passthrough ? "yes" : "no");
	ESP_LOGI(TAG, "  HTTP port: %u, host name: %s", s->http_port, s->hostname);
	ESP_LOGI(TAG, "  Debug BLE: %s", s->debug_ble ? "yes" : "no");
	ESP_LOGI(TAG, "  Stream: %u fps, resume after %u ms", s->stream_fps, s->stream_resume_ms);

	if (storage_init() != ESP_OK)
		ESP_LOGE(TAG, "scratch partition unavailable: uploads will fail");
	ESP_ERROR_CHECK(wifi_start());
	log_heap("Wi-Fi up");
	ESP_ERROR_CHECK(display_init());
	log_heap("BLE up");
	ESP_ERROR_CHECK(http_start());
	log_heap("HTTP up");
}
