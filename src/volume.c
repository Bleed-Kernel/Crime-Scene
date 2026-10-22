#include <fcntl.h>
#include <syscalls.h>
#include <audio.h>
#include <volume.h>

#define AUDIO_PATH "/dev/audio0"

static int audio_fd = -1;

static int audio_open(void) {
    if (audio_fd < 0)
        audio_fd = _open(AUDIO_PATH, O_RDONLY);
    return audio_fd >= 0;
}

int volume_read(int *percent, int *mute) {
    struct audio_volume vol;
    if (!audio_open() || _ioctl(audio_fd, AUDIO_IOC_GET_VOLUME, &vol) != 0)
        return 0;

    *percent = vol.left > vol.right ? vol.left : vol.right;
    *mute    = vol.mute;
    return 1;
}

void volume_write(int percent, int mute) {
    if (percent < 0)   percent = 0;
    if (percent > 100) percent = 100;

    struct audio_volume vol = { (uint8_t)percent, (uint8_t)percent, (uint8_t)(mute ? 1 : 0), 0 };
    if (audio_open())
        _ioctl(audio_fd, AUDIO_IOC_SET_VOLUME, &vol);
}
