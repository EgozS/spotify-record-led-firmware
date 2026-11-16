#pragma once
#include <stdint.h>
#include <stdbool.h>

#define ART_SIZE 64  // album art is delivered as 64x64 RGB888

struct NetState {
    bool wifi_connected;
    bool has_track;     // something (song/episode) is loaded in Spotify
    bool playing;       // ...and it is actually playing right now
    bool has_art;       // art_version refers to a valid image
    bool setup_mode;    // the setup hotspot is open (needs Wi-Fi / Spotify details)
    uint32_t art_version;
};

// Starts Wi-Fi and the Spotify polling task. Non-blocking.
void net_start(void);

// Thread-safe snapshot of the current state.
NetState net_get_state(void);

// Copies the current album art (ART_SIZE*ART_SIZE*3 bytes, RGB) into dst.
// Returns the version of the copied image.
uint32_t net_copy_art(uint8_t *dst);
