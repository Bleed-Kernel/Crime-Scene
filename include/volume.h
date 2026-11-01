#pragma once

#include <audio.h>

int  volume_read(int *percent, int *mute);
void volume_write(int percent, int mute);

// 0 when the card has no choice of outputs
int  volume_outputs(struct audio_outputs *outputs);
void volume_set_output(int index);
