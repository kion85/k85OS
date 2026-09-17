#pragma once
void k85_run_music_player(void);

// Проигрывает конкретный аудиофайл (.wav 16-бит PCM или .mp3) тем же
// экраном плеера, что и обычный выбор из списка Music Player. Расширение
// определяет, какой декодер использовать.
void k85_play_audio_file(const char *path);
