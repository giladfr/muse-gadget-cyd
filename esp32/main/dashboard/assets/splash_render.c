// Host tool for gen_splash.py: renders Muse's mascot (avatar/muse_pixel.c)
// through the boot pop and a happy wave, and writes each frame's 64x64
// RGB565 grid to stdout.
//
//   splash_render FPS SECONDS > frames.raw
#include <stdio.h>
#include <stdlib.h>

#include "muse_pixel.h"

int main(int argc, char **argv) {
    int fps = argc > 1 ? atoi(argv[1]) : 15;
    float secs = argc > 2 ? (float)atof(argv[2]) : 3.0f;
    srand(7);
    muse_pixel_set_size(MUSE_PX_W);
    uint16_t row[MUSE_PX_W];
    for (int f = 0; f < (int)(secs * fps + 0.5f); f++) {
        float t = (float)f / fps;
        // Boot: pops up from a squash and opens their eyes (1.4 s), then
        // cheers with hearts.
        muse_pose_t p = {.mode = t < 1.5f ? MUSE_MODE_BOOT : MUSE_MODE_IDLE,
                         .t = t + 0.001f,
                         .mode_t = t < 1.5f ? t : t - 1.5f};
        p.happy = t < 1.5f ? 0 : (t - 1.5f) / 0.3f > 1 ? 1 : (t - 1.5f) / 0.3f;
        muse_pixel_render(&p);
        for (int y = 0; y < MUSE_PX_H; y++) {
            muse_pixel_scale(row, MUSE_PX_W, 0, MUSE_PX_W - 1, y, y);
            fwrite(row, sizeof(row), 1, stdout);
        }
    }
    return 0;
}
