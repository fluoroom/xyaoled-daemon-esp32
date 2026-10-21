// Display logic, the counterpart of reference/go-daemon/display.go: turns
// uploads into the panel's GIF (through the image pipeline and the
// scratch partition), keeps the connection up, and holds the brightness,
// manual-off and streaming state.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "img.h"

#define DISPLAY_ERR_BASE 0x7200
#define DISPLAY_ERR_STREAMING (DISPLAY_ERR_BASE + 1)  // a /stream holds the panel (409)
#define DISPLAY_ERR_SCREEN_OFF (DISPLAY_ERR_BASE + 2) // brightness <= 5: images are ignored
#define DISPLAY_ERR_MANUAL_OFF (DISPLAY_ERR_BASE + 3) // /off: brightness changes refused (409)
#define DISPLAY_ERR_IMAGE (DISPLAY_ERR_BASE + 4)      // the upload could not be decoded

typedef enum { DISPLAY_AUTO, DISPLAY_STATIC, DISPLAY_ANIMATED } display_kind_t;

esp_err_t display_init(void);

// The text for an error from the calls below, with the image decoder's
// reason for DISPLAY_ERR_IMAGE.
const char *display_error_text(esp_err_t err);

// Image sends. r is read to its end (or until the image ends).
// display_show replaces the playlist (/display, /upload).
esp_err_t display_show(const img_reader_t *r, display_kind_t kind);
// display_live shows an image at once without touching the playlist.
esp_err_t display_live(const img_reader_t *r);
// The same for /stream frames, decoded in RAM (no flash writes).
esp_err_t display_live_mem(const uint8_t *data, size_t len);

// Playlist: items are encoded as they arrive, then sent together.
typedef struct display_playlist display_playlist_t;
esp_err_t display_playlist_begin(display_playlist_t **pl);
esp_err_t display_playlist_add(display_playlist_t *pl, const img_reader_t *r);
// Sends the items when send is set; always frees pl. *items gets the count.
esp_err_t display_playlist_finish(display_playlist_t *pl, bool send, int *items);

esp_err_t display_clear_screen(void);
esp_err_t display_resume(void);        // refused while streaming
esp_err_t display_stream_resume(void); // the streamer's own resume
esp_err_t display_clear_playlist(void);

esp_err_t display_set_brightness(uint8_t level); // saved for the next boot
esp_err_t display_set_brightness_unless_off(uint8_t level);
esp_err_t display_turn_off(void); // brightness 5, and brightness changes refused
esp_err_t display_turn_on(uint8_t level);
uint8_t display_brightness(void);

esp_err_t display_power(bool on);
esp_err_t display_raw(const uint8_t *body, size_t len);
esp_err_t display_connect(void);
void display_disconnect(void);
void display_reconnect(void); // in the background

void display_set_streaming(bool on);
bool display_streaming(void);
bool display_connected(void);
