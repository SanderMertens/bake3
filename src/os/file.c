#include "bake/os.h"

static int bake_file_close(FILE *f, const char *path) {
    if (fclose(f) != 0) {
        bake_log_last_errno("close file", path);
        return -1;
    }
    return 0;
}

char* bake_file_read(const char *path, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        if (errno != ENOENT) {
            bake_log_last_errno("open file for reading", path);
        }
        return NULL;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        bake_log_last_errno("seek file", path);
        bake_file_close(f, path);
        return NULL;
    }

    long len = ftell(f);
    if (len < 0) {
        bake_log_last_errno("tell file position", path);
        bake_file_close(f, path);
        return NULL;
    }

    if (fseek(f, 0, SEEK_SET) != 0) {
        bake_log_last_errno("seek file", path);
        bake_file_close(f, path);
        return NULL;
    }

    char *buf = ecs_os_malloc((size_t)len + 1);
    if (!buf) {
        bake_file_close(f, path);
        return NULL;
    }

    size_t read_len = fread(buf, 1, (size_t)len, f);
    if (read_len != (size_t)len) {
        if (ferror(f)) {
            bake_log_last_errno("read file", path);
        } else {
            ecs_err("failed to read file '%s': unexpected end of file", path);
        }
        bake_file_close(f, path);
        ecs_os_free(buf);
        return NULL;
    }

    if (bake_file_close(f, path) != 0) {
        ecs_os_free(buf);
        return NULL;
    }

    buf[len] = '\0';
    if (len_out) {
        *len_out = (size_t)len;
    }
    return buf;
}

static bool bake_file_content_matches(const char *path, const char *content, size_t len) {
    if (!bake_path_exists(path)) {
        return false;
    }
    size_t existing_len = 0;
    char *existing = bake_file_read(path, &existing_len);
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

    char *dir = bake_dirname(path);
    if (!dir) {
        return -1;
    }

    if (bake_mkdirs(dir) != 0) {
        ecs_os_free(dir);
        return -1;
    }
    ecs_os_free(dir);

    size_t len = strlen(content);
    if (bake_file_content_matches(path, content, len)) {
        return 0;
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        bake_log_last_errno("open file for writing", path);
        return -1;
    }

    size_t written = fwrite(content, 1, len, f);
    if (written != len) {
        bake_log_last_errno("write file", path);
        bake_file_close(f, path);
        return -1;
    }

    return bake_file_close(f, path);
}

int64_t bake_file_mtime(const char *path) {
    return bake_os_file_mtime(path);
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

int bake_file_copy(const char *src, const char *dst) {
    size_t len = 0;
    char *content = bake_file_read(src, &len);
    if (!content) {
        if (!bake_path_exists(src)) {
            ecs_err("failed to copy '%s' to '%s': source file does not exist", src, dst);
        }
        return -1;
    }

    char *dir = bake_dirname(dst);
    if (!dir || bake_mkdirs(dir) != 0) {
        ecs_os_free(content);
        ecs_os_free(dir);
        return -1;
    }
    ecs_os_free(dir);

    if (bake_file_content_matches(dst, content, len)) {
        ecs_os_free(content);
        return bake_file_sync_mode(src, dst);
    }

    FILE *f = fopen(dst, "wb");
    if (!f) {
        bake_log_last_errno("open file for writing", dst);
        ecs_os_free(content);
        return -1;
    }

    size_t written = fwrite(content, 1, len, f);
    if (written != len) {
        bake_log_last_errno("write file", dst);
        bake_file_close(f, dst);
        ecs_os_free(content);
        return -1;
    }

    if (bake_file_close(f, dst) != 0) {
        ecs_os_free(content);
        return -1;
    }

    ecs_os_free(content);
    return bake_file_sync_mode(src, dst);
}
