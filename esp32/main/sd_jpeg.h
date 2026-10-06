// sd_jpeg.h — display a baseline JPEG from the SD card full-screen.
#pragma once

// Draw /sdcard/<path> centered on the display and enter takeover mode.
// Returns NULL on success, or a static error string.
const char *sd_jpeg_show(const char *path);
