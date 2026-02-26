#include "bake/os.h"

char* bake_file_read(const char *path, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }

    long len = ftell(f);
    if (len < 0) {
        fclose(f);
        return NULL;
    }

    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }

    char *buf = ecs_os_malloc((size_t)len + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }

    size_t read_len = fread(buf, 1, (size_t)len, f);
    fclose(f);
    if (read_len != (size_t)len) {
        ecs_os_free(buf);
        return NULL;
    }

    buf[len] = '\0';
    if (len_out) {
        *len_out = (size_t)len;
    }
    return buf;
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
    if (bake_path_exists(path)) {
        size_t existing_len = 0;
        char *existing = bake_file_read(path, &existing_len);
        if (existing) {
            if (existing_len == len && !memcmp(existing, content, len)) {
                ecs_os_free(existing);
                return 0;
            }
            ecs_os_free(existing);
        }
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        return -1;
    }

    size_t written = fwrite(content, 1, len, f);
    fclose(f);
    return written == len ? 0 : -1;
}

int64_t bake_file_mtime(const char *path) {
    return bake_os_file_mtime(path);
}

int bake_copy_file(const char *src, const char *dst) {
    size_t len = 0;
    char *content = bake_file_read(src, &len);
    if (!content) {
        return -1;
    }

    char *dir = bake_dirname(dst);
    if (!dir || bake_mkdirs(dir) != 0) {
        ecs_os_free(content);
        ecs_os_free(dir);
        return -1;
    }
    ecs_os_free(dir);

    if (bake_path_exists(dst)) {
        size_t existing_len = 0;
        char *existing = bake_file_read(dst, &existing_len);
        if (existing) {
            if (existing_len == len && !memcmp(existing, content, len)) {
                ecs_os_free(existing);
                ecs_os_free(content);
                return 0;
            }
            ecs_os_free(existing);
        }
    }

    FILE *f = fopen(dst, "wb");
    if (!f) {
        ecs_os_free(content);
        return -1;
    }

    size_t written = fwrite(content, 1, len, f);
    fclose(f);
    ecs_os_free(content);
    return written == len ? 0 : -1;
}
