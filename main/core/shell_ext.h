#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef void (*k85_shell_fn)(const char *line, char *out, size_t out_size);

// Pipes (cmd1 | cmd2) and redirects (> file, >> file). Lines without them go straight to run().
void k85_shell_pipeline(const char *line, char *out, size_t out_size, k85_shell_fn run);

// Extra commands: grep head tail wc cp mv rm mkdir touch date df du hostname ifconfig
// nslookup wget curl watch neofetch at tar scrprobe. Returns true if the command was handled.
bool k85_shell_ext_run(const char *cmd, const char *arg, char *out, size_t out_size);
