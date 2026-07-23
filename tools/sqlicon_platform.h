#ifndef SQLICON_PLATFORM_H
#define SQLICON_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <time.h>

int sqlicon_platform_install_signal_handlers(void);
void sqlicon_platform_reset_interrupt_state(void);
int sqlicon_platform_is_stdin_tty(void);
int sqlicon_platform_clock_now(struct timespec *ts);
long sqlicon_platform_process_id(void);

char *sqlicon_platform_readline(const char *prompt);
void sqlicon_platform_history_load(void);
void sqlicon_platform_history_save(void);
void sqlicon_platform_history_add(const char *line);

int sqlicon_platform_config_dir(char *out, size_t out_cap);
int sqlicon_platform_mkdir_p(const char *path);
int sqlicon_platform_restrict_dir(const char *path);
int sqlicon_platform_restrict_file(const char *path);
int sqlicon_platform_replace_file(const char *src_path, const char *dst_path);
int sqlicon_platform_profile_key_material(char *out, size_t out_cap);

#endif /* SQLICON_PLATFORM_H */
