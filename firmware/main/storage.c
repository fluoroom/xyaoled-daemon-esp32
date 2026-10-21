#include "storage.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "esp_littlefs.h"
#include "esp_log.h"

static const char *TAG = "storage";
static const char *LABEL = "scratch";

esp_err_t storage_init(void)
{
	esp_vfs_littlefs_conf_t conf = {
		.base_path = SCRATCH,
		.partition_label = LABEL,
		.format_if_mount_failed = true,
	};
	esp_err_t err = esp_vfs_littlefs_register(&conf);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "mount failed: %s", esp_err_to_name(err));
		return err;
	}
	storage_clean();
	size_t total, used;
	storage_usage(&total, &used);
	ESP_LOGI(TAG, "%s: %u KB, %u KB used", SCRATCH, (unsigned)(total / 1024), (unsigned)(used / 1024));
	return ESP_OK;
}

void storage_clean(void)
{
	DIR *d = opendir(SCRATCH);
	if (!d)
		return;
	struct dirent *e;
	char path[300];
	while ((e = readdir(d)) != NULL) {
		if (e->d_name[0] == '.')
			continue;
		snprintf(path, sizeof path, SCRATCH "/%s", e->d_name);
		unlink(path);
	}
	closedir(d);
}

void storage_usage(size_t *total, size_t *used)
{
	*total = *used = 0;
	esp_littlefs_info(LABEL, total, used);
}

size_t storage_free(void)
{
	size_t total, used;
	storage_usage(&total, &used);
	return total > used ? total - used : 0;
}
