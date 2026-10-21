// What the simulated panel (QEMU build) received, for the integration tests.
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
	int uploads, packets, selects, clears, resumes, header_errors;
	uint8_t brightness;
	bool power;
} xy_panel_sim_stats_t;

// The last GIF the panel received completely, with its trailer and program.
const uint8_t *xy_panel_sim_last(uint32_t *len, uint8_t trailer[8], uint8_t *program);
void xy_panel_sim_stats(xy_panel_sim_stats_t *out);
