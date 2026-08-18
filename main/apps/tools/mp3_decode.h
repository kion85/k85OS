#pragma once

struct K85Mp3Info {
    int sample_rate;
    int channels;
    double duration_sec_est;
};

bool k85_mp3_open(const char *path, K85Mp3Info *info);
bool k85_mp3_decode_and_feed_next_frame(double *position_sec_out);
void k85_mp3_seek_relative(double delta_sec);
void k85_mp3_close(void);
