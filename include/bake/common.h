#ifndef BAKE3_COMMON_H
#define BAKE3_COMMON_H

#if defined(__linux__) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE
#endif

#if defined(__linux__) && !defined(_FILE_OFFSET_BITS)
/* Use large-file/direntry variants on 32-bit Linux so mounted filesystems
 * with large inode values don't break stat/readdir. */
#define _FILE_OFFSET_BITS 64
#endif

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#define strcasecmp _stricmp
#else
#include <strings.h>
#endif
#include <stdarg.h>

#define BAKE_UNUSED(x) (void)(x)

int bake_run_command(const char *cmd, bool log_command);
char* bake_text_replace(const char *input, const char *needle, const char *replacement);

char* bake_project_id_as_dash(const char *id);
char* bake_macro_upper(const char *value);

bool bake_has_suffix(const char *value, const char *suffix);
bool bake_char_is_space(char ch);
char* bake_project_id_as_macro(const char *id);
char* bake_project_id_base(const char *id);

#endif
