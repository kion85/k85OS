#include "mp3_decode.h"
#include "M5Unified.h"
#include "mp3dec.h"
#include "esp_log.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>

static const char *TAG = "k85_mp3";

#define READBUF_SIZE (1024 * 10)
#define REFILL_THRESHOLD 2600 // запас больше MAINBUF_SIZE (1940) у Helix

static FILE *s_file = nullptr;
static HMP3Decoder s_dec = nullptr;
static unsigned char *s_inbuf = nullptr;
static unsigned char *s_read_ptr = nullptr;
static int s_bytes_left = 0;
static long s_file_size = 0;
static double s_bytes_per_sec_est = 0;
static double s_cumulative_sec = 0;
static bool s_have_bitrate_est = false;

static short s_pcm_buf[1152 * 2];

static void refill_if_needed(void) {
    if (s_bytes_left >= REFILL_THRESHOLD || !s_file) return;
    if (s_bytes_left > 0 && s_read_ptr != s_inbuf) {
        memmove(s_inbuf, s_read_ptr, s_bytes_left);
    }
    s_read_ptr = s_inbuf;
    int n = (int)fread(s_inbuf + s_bytes_left, 1, READBUF_SIZE - s_bytes_left, s_file);
    if (n > 0) s_bytes_left += n;
}

bool k85_mp3_open(const char *path, K85Mp3Info *info) {
    k85_mp3_close();

    s_file = fopen(path, "rb");
    if (!s_file) return false;

    fseek(s_file, 0, SEEK_END);
    s_file_size = ftell(s_file);
    fseek(s_file, 0, SEEK_SET);

    s_inbuf = (unsigned char *)malloc(READBUF_SIZE);
    if (!s_inbuf) { fclose(s_file); s_file = nullptr; return false; }

    s_dec = MP3InitDecoder();
    if (!s_dec) { free(s_inbuf); s_inbuf = nullptr; fclose(s_file); s_file = nullptr; return false; }

    s_bytes_left = 0;
    s_read_ptr = s_inbuf;
    s_cumulative_sec = 0;
    s_have_bitrate_est = false;
    s_bytes_per_sec_est = 0;

    refill_if_needed();

    int offset = MP3FindSyncWord(s_read_ptr, s_bytes_left);
    if (offset < 0) { k85_mp3_close(); return false; }
    s_read_ptr += offset;
    s_bytes_left -= offset;

    MP3FrameInfo fi;
    if (MP3GetNextFrameInfo(s_dec, &fi, s_read_ptr) != 0) { k85_mp3_close(); return false; }

    info->sample_rate = fi.samprate;
    info->channels = fi.nChans;
    if (fi.bitrate > 0) {
        s_bytes_per_sec_est = fi.bitrate / 8.0;
        s_have_bitrate_est = true;
        info->duration_sec_est = s_file_size / s_bytes_per_sec_est;
    } else {
        info->duration_sec_est = 0;
    }
    return true;
}

bool k85_mp3_decode_and_feed_next_frame(double *position_sec_out) {
    if (!s_dec || !s_file) return false;

    refill_if_needed();
    if (s_bytes_left <= 0) return false;

    int offset = MP3FindSyncWord(s_read_ptr, s_bytes_left);
    if (offset < 0) return false;
    s_read_ptr += offset;
    s_bytes_left -= offset;
    if (s_bytes_left <= 0) return false;

    MP3FrameInfo fi;
    if (MP3GetNextFrameInfo(s_dec, &fi, s_read_ptr) != 0) return false;

    int err = MP3Decode(s_dec, &s_read_ptr, &s_bytes_left, s_pcm_buf, 0);
    if (err != 0) {
        // повреждённый фрейм — сдвигаемся на байт и пробуем дальше на след. итерации
        if (s_bytes_left > 0) { s_read_ptr++; s_bytes_left--; }
        return true;
    }

    if (!s_have_bitrate_est && fi.bitrate > 0) {
        s_bytes_per_sec_est = fi.bitrate / 8.0;
        s_have_bitrate_est = true;
    }

    M5.Speaker.playRaw(s_pcm_buf, fi.outputSamps, fi.samprate, fi.nChans == 2, 1, -1, false);

    double frame_sec = (double)(fi.outputSamps / fi.nChans) / fi.samprate;
    s_cumulative_sec += frame_sec;
    if (position_sec_out) *position_sec_out = s_cumulative_sec;
    return true;
}

void k85_mp3_seek_relative(double delta_sec) {
    if (!s_file || s_bytes_per_sec_est <= 0) return;

    long cur_file_pos = ftell(s_file) - s_bytes_left;
    long delta_bytes = (long)(delta_sec * s_bytes_per_sec_est);
    long new_pos = cur_file_pos + delta_bytes;
    if (new_pos < 0) new_pos = 0;
    if (new_pos > s_file_size) new_pos = s_file_size;

    fseek(s_file, new_pos, SEEK_SET);
    s_bytes_left = 0;
    s_read_ptr = s_inbuf;
    s_cumulative_sec += delta_sec;
    if (s_cumulative_sec < 0) s_cumulative_sec = 0;

    refill_if_needed();
    int offset = MP3FindSyncWord(s_read_ptr, s_bytes_left);
    if (offset >= 0) {
        s_read_ptr += offset;
        s_bytes_left -= offset;
    }
}

void k85_mp3_close(void) {
    if (s_dec) { MP3FreeDecoder(s_dec); s_dec = nullptr; }
    if (s_inbuf) { free(s_inbuf); s_inbuf = nullptr; }
    if (s_file) { fclose(s_file); s_file = nullptr; }
    s_bytes_left = 0;
    s_read_ptr = nullptr;
}
