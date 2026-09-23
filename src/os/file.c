#include "bake/os.h"
#include <flecs.h>

static int bake_file_close(FILE *f, const char *path, bool log_errors) {
    if (fclose(f) != 0) {
        if (log_errors) {
            bake_log_errno_last("close file", path);
        }
        return -1;
    }
    return 0;
}

static char* bake_file_read_impl(
    const char *path,
    size_t *len_out,
    bool log_errors)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        if (log_errors && errno != ENOENT) {
            bake_log_errno_last("open file for reading", path);
        }
        return NULL;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        if (log_errors) {
            bake_log_errno_last("seek file", path);
        }
        bake_file_close(f, path, log_errors);
        return NULL;
    }

    long len = ftell(f);
    if (len < 0) {
        if (log_errors) {
            bake_log_errno_last("tell file position", path);
        }
        bake_file_close(f, path, log_errors);
        return NULL;
    }

    if (fseek(f, 0, SEEK_SET) != 0) {
        if (log_errors) {
            bake_log_errno_last("seek file", path);
        }
        bake_file_close(f, path, log_errors);
        return NULL;
    }

    char *buf = ecs_os_malloc((size_t)len + 1);
    if (!buf) {
        bake_file_close(f, path, log_errors);
        return NULL;
    }

    size_t read_len = fread(buf, 1, (size_t)len, f);
    if (read_len != (size_t)len) {
        if (log_errors) {
            if (ferror(f)) {
                bake_log_errno_last("read file", path);
            } else {
                ecs_err("failed to read file '%s': unexpected end of file", path);
            }
        }
        bake_file_close(f, path, log_errors);
        ecs_os_free(buf);
        return NULL;
    }

    if (bake_file_close(f, path, log_errors) != 0) {
        ecs_os_free(buf);
        return NULL;
    }

    buf[len] = '\0';
    if (len_out) {
        *len_out = (size_t)len;
    }
    return buf;
}

char* bake_file_read(const char *path, size_t *len_out) {
    return bake_file_read_impl(path, len_out, true);
}

static bool bake_file_content_matches(
    const char *path,
    const char *content,
    size_t len,
    bool log_errors)
{
    int64_t existing_size = bake_os_file_size(path);
    if (existing_size < 0 || (size_t)existing_size != len) {
        return false;
    }
    size_t existing_len = 0;
    char *existing = bake_file_read_impl(path, &existing_len, log_errors);
    if (!existing) {
        return false;
    }
    bool matches = (existing_len == len && !memcmp(existing, content, len));
    ecs_os_free(existing);
    return matches;
}

int bake_file_write(const char *path, const char *content) {
    if (!path || !content) {
        return -1;
    }

    char *dir = bake_path_dirname(path);
    if (!dir) {
        return -1;
    }

    if (bake_os_mkdirs(dir) != 0) {
        ecs_os_free(dir);
        return -1;
    }
    ecs_os_free(dir);

    size_t len = strlen(content);
    if (bake_file_content_matches(path, content, len, true)) {
        return 0;
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        bake_log_errno_last("open file for writing", path);
        return -1;
    }

    size_t written = fwrite(content, 1, len, f);
    if (written != len) {
        bake_log_errno_last("write file", path);
        bake_file_close(f, path, true);
        return -1;
    }

    return bake_file_close(f, path, true);
}

char* bake_file_read_trimmed(const char *path) {
    size_t len = 0;
    char *text = bake_file_read(path, &len);
    if (!text) {
        return NULL;
    }

    while (len > 0) {
        char ch = text[len - 1];
        if (ch != '\n' && ch != '\r') {
            break;
        }
        text[len - 1] = '\0';
        len--;
    }

    return text;
}

int bake_os_file_copy(const char *src, const char *dst) {
    size_t len = 0;
    char *content = bake_file_read_impl(src, &len, false);
    if (!content) {
        ecs_warn("failed to copy '%s' to '%s', skipping", src, dst);
        return 0;
    }

    char *dir = bake_path_dirname(dst);
    if (!dir || bake_os_mkdirs_silent(dir) != 0) {
        ecs_os_free(content);
        ecs_os_free(dir);
        ecs_warn("failed to copy '%s' to '%s', skipping", src, dst);
        return 0;
    }
    ecs_os_free(dir);

    if (bake_file_content_matches(dst, content, len, false)) {
        ecs_os_free(content);
        if (bake_file_sync_mode_silent(src, dst) != 0) {
            ecs_warn("failed to copy '%s' to '%s', skipping", src, dst);
        }
        return 0;
    }

    FILE *f = fopen(dst, "wb");
    if (!f) {
        ecs_os_free(content);
        ecs_warn("failed to copy '%s' to '%s', skipping", src, dst);
        return 0;
    }

    size_t written = fwrite(content, 1, len, f);
    if (written != len) {
        fclose(f);
        ecs_os_free(content);
        ecs_warn("failed to copy '%s' to '%s', skipping", src, dst);
        return 0;
    }

    if (fclose(f) != 0) {
        ecs_os_free(content);
        ecs_warn("failed to copy '%s' to '%s', skipping", src, dst);
        return 0;
    }

    ecs_os_free(content);
    if (bake_file_sync_mode_silent(src, dst) != 0) {
        ecs_warn("failed to copy '%s' to '%s', skipping", src, dst);
    }
    return 0;
}
