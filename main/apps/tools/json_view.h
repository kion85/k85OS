#pragma once
// K85JV1: JSON viewer (any JSON, lenient parser) and a normalizer for the game interpreter
#include <stddef.h>
#include <stdbool.h>
void k85_json_view_file(const char *path);
void k85_json_view_buffer(const char *buf, size_t n, const char *title);
// lenient JSON -> strict compact JSON in a new buffer (free() it); false = nothing could be parsed
bool k85_json_normalize(const char *src, size_t n, char **out, size_t *outn);