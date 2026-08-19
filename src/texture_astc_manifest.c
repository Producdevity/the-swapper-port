#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

typedef unsigned long z_ulong;
typedef int (*z_uncompress_fn)(unsigned char *, z_ulong *, const unsigned char *, z_ulong);
typedef int (*z_compress2_fn)(unsigned char *, z_ulong *, const unsigned char *, z_ulong, int);
typedef z_ulong (*z_compress_bound_fn)(z_ulong);
typedef z_ulong (*z_crc32_fn)(z_ulong, const unsigned char *, unsigned int);

struct z_api {
    void *handle;
    z_uncompress_fn uncompress;
    z_compress2_fn compress2;
    z_compress_bound_fn compress_bound;
    z_crc32_fn crc32;
};

struct buffer {
    unsigned char *data;
    size_t size;
    size_t capacity;
};

struct image {
    uint32_t width;
    uint32_t height;
    unsigned char *rgba;
};

struct astc_source {
    uint64_t hash;
    uint32_t width;
    uint32_t height;
    char block[4];
    char relative_path[PATH_MAX];
};

enum texture_kind {
    TEXTURE_SKIP,
    TEXTURE_NORMAL,
    TEXTURE_COLOR,
    TEXTURE_DETAIL,
    TEXTURE_FONT
};

static struct astc_source *astc_sources;
static size_t astc_source_count;

static int mkdir_p_for_file(const char *path);

static uint32_t read_be32(const unsigned char *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint32_t read_le32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t read_le16(const unsigned char *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static void write_be32(unsigned char *p, uint32_t value) {
    p[0] = (unsigned char)(value >> 24);
    p[1] = (unsigned char)(value >> 16);
    p[2] = (unsigned char)(value >> 8);
    p[3] = (unsigned char)value;
}

static int buffer_reserve(struct buffer *buffer, size_t extra) {
    size_t needed = buffer->size + extra;
    size_t capacity = buffer->capacity == 0 ? 4096 : buffer->capacity;
    unsigned char *data;

    if (needed < buffer->size)
        return -1;

    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2)
            return -1;
        capacity *= 2;
    }

    if (capacity == buffer->capacity)
        return 0;

    data = realloc(buffer->data, capacity);
    if (data == NULL)
        return -1;

    buffer->data = data;
    buffer->capacity = capacity;
    return 0;
}

static int buffer_append(struct buffer *buffer, const void *data, size_t size) {
    if (buffer_reserve(buffer, size) != 0)
        return -1;

    memcpy(buffer->data + buffer->size, data, size);
    buffer->size += size;
    return 0;
}

static void buffer_free(struct buffer *buffer) {
    free(buffer->data);
    buffer->data = NULL;
    buffer->size = 0;
    buffer->capacity = 0;
}

static int read_file(const char *path, struct buffer *buffer) {
    FILE *file = fopen(path, "rb");
    long size;

    if (file == NULL)
        return -1;

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return -1;
    }

    size = ftell(file);
    if (size < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return -1;
    }

    buffer->data = malloc((size_t)size);
    if (buffer->data == NULL && size != 0) {
        fclose(file);
        return -1;
    }

    buffer->size = (size_t)size;
    buffer->capacity = (size_t)size;

    if (size != 0 && fread(buffer->data, 1, (size_t)size, file) != (size_t)size) {
        fclose(file);
        buffer_free(buffer);
        return -1;
    }

    return fclose(file);
}

static int load_zlib(struct z_api *z) {
    memset(z, 0, sizeof(*z));

    z->handle = dlopen("libz.so.1", RTLD_LAZY);
    if (z->handle == NULL)
        z->handle = dlopen("libz.so", RTLD_LAZY);
    if (z->handle == NULL)
        return -1;

    z->uncompress = (z_uncompress_fn)dlsym(z->handle, "uncompress");
    z->compress2 = (z_compress2_fn)dlsym(z->handle, "compress2");
    z->compress_bound = (z_compress_bound_fn)dlsym(z->handle, "compressBound");
    z->crc32 = (z_crc32_fn)dlsym(z->handle, "crc32");
    return z->uncompress == NULL || z->compress2 == NULL || z->compress_bound == NULL ||
                   z->crc32 == NULL
               ? -1
               : 0;
}

static int paeth(int a, int b, int c) {
    int p = a + b - c;
    int pa = abs(p - a);
    int pb = abs(p - b);
    int pc = abs(p - c);

    if (pa <= pb && pa <= pc)
        return a;
    if (pb <= pc)
        return b;
    return c;
}

static int decode_png_rgba8(const char *path, const struct z_api *z, struct image *image) {
    static const unsigned char signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    struct buffer png = {0};
    struct buffer idat = {0};
    unsigned char *scanlines = NULL;
    unsigned char *decoded = NULL;
    z_ulong scanline_size;
    size_t offset = 8;
    size_t row_size;
    uint32_t width = 0;
    uint32_t height = 0;
    unsigned int components = 0;
    int result = -1;

    memset(image, 0, sizeof(*image));

    if (read_file(path, &png) != 0)
        return -1;

    if (png.size < sizeof(signature) || memcmp(png.data, signature, sizeof(signature)) != 0)
        goto done;

    while (offset + 12 <= png.size) {
        uint32_t length = read_be32(png.data + offset);
        const unsigned char *type = png.data + offset + 4;
        const unsigned char *chunk = png.data + offset + 8;

        offset += 8;
        if (length > png.size - offset - 4)
            goto done;

        if (memcmp(type, "IHDR", 4) == 0) {
            if (length != 13)
                goto done;

            width = read_be32(chunk);
            height = read_be32(chunk + 4);
            if (width == 0 || height == 0 || chunk[8] != 8 ||
                (chunk[9] != 2 && chunk[9] != 6) || chunk[10] != 0 || chunk[11] != 0 ||
                chunk[12] != 0)
                goto done;
            components = chunk[9] == 6 ? 4 : 3;
        } else if (memcmp(type, "IDAT", 4) == 0) {
            if (buffer_append(&idat, chunk, length) != 0)
                goto done;
        } else if (memcmp(type, "IEND", 4) == 0) {
            break;
        }

        offset += (size_t)length + 4;
    }

    if (width == 0 || height == 0 || components == 0 || idat.size == 0)
        goto done;
    if (width > UINT32_MAX / components || width > UINT32_MAX / 4)
        goto done;

    row_size = (size_t)width * components;
    if (height > SIZE_MAX / (row_size + 1) || height > SIZE_MAX / ((size_t)width * 4))
        goto done;

    scanline_size = (z_ulong)height * (z_ulong)(row_size + 1);
    scanlines = malloc((size_t)scanline_size);
    decoded = malloc(row_size * height);
    image->rgba = malloc((size_t)width * height * 4);
    if (scanlines == NULL || decoded == NULL || image->rgba == NULL)
        goto done;

    if (z->uncompress(scanlines, &scanline_size, idat.data, (z_ulong)idat.size) != 0)
        goto done;
    if (scanline_size != (z_ulong)height * (z_ulong)(row_size + 1))
        goto done;

    for (uint32_t y = 0; y < height; y++) {
        const unsigned char *src = scanlines + (size_t)y * (row_size + 1);
        unsigned char *dst = decoded + (size_t)y * row_size;
        const unsigned char *prev = y == 0 ? NULL : decoded + (size_t)(y - 1) * row_size;
        int filter = src[0];

        src++;
        if (filter < 0 || filter > 4)
            goto done;

        for (size_t x = 0; x < row_size; x++) {
            int left = x >= components ? dst[x - components] : 0;
            int up = prev == NULL ? 0 : prev[x];
            int up_left = (prev != NULL && x >= components) ? prev[x - components] : 0;
            int value = src[x];

            switch (filter) {
            case 1:
                value += left;
                break;
            case 2:
                value += up;
                break;
            case 3:
                value += (left + up) / 2;
                break;
            case 4:
                value += paeth(left, up, up_left);
                break;
            default:
                break;
            }

            dst[x] = (unsigned char)(value & 0xff);
        }
    }

    for (uint64_t i = 0; i < (uint64_t)width * height; i++) {
        const unsigned char *src = decoded + i * components;
        unsigned char *dst = image->rgba + i * 4;

        dst[0] = src[0];
        dst[1] = src[1];
        dst[2] = src[2];
        dst[3] = components == 4 ? src[3] : 255;
    }

    image->width = width;
    image->height = height;
    result = 0;

done:
    if (result != 0) {
        free(image->rgba);
        memset(image, 0, sizeof(*image));
    }
    free(decoded);
    free(scanlines);
    buffer_free(&idat);
    buffer_free(&png);
    return result;
}

static int decode_tga_rgba8(const char *path, struct image *image) {
    struct buffer tga = {0};
    size_t offset;
    size_t pixel_size;
    uint32_t width;
    uint32_t height;
    unsigned int components;
    int top_origin;
    int right_origin;
    int result = -1;

    memset(image, 0, sizeof(*image));
    if (read_file(path, &tga) != 0)
        return -1;
    if (tga.size < 18 || tga.data[1] != 0 || tga.data[2] != 2)
        goto done;

    width = read_le16(tga.data + 12);
    height = read_le16(tga.data + 14);
    components = tga.data[16] == 32 ? 4 : tga.data[16] == 24 ? 3 : 0;
    if (width == 0 || height == 0 || components == 0 || width > UINT32_MAX / 4 ||
        height > SIZE_MAX / ((size_t)width * 4)) {
        goto done;
    }

    offset = 18 + tga.data[0];
    pixel_size = (size_t)width * height * components;
    if (offset > tga.size || pixel_size > tga.size - offset)
        goto done;

    image->rgba = malloc((size_t)width * height * 4);
    if (image->rgba == NULL)
        goto done;

    top_origin = (tga.data[17] & 0x20) != 0;
    right_origin = (tga.data[17] & 0x10) != 0;
    for (uint32_t y = 0; y < height; y++) {
        uint32_t dst_y = top_origin ? y : height - 1 - y;
        for (uint32_t x = 0; x < width; x++) {
            uint32_t dst_x = right_origin ? width - 1 - x : x;
            const unsigned char *src = tga.data + offset + ((size_t)y * width + x) * components;
            unsigned char *dst = image->rgba + ((size_t)dst_y * width + dst_x) * 4;

            dst[0] = src[2];
            dst[1] = src[1];
            dst[2] = src[0];
            dst[3] = components == 4 ? src[3] : 255;
        }
    }

    image->width = width;
    image->height = height;
    result = 0;

done:
    if (result != 0) {
        free(image->rgba);
        memset(image, 0, sizeof(*image));
    }
    buffer_free(&tga);
    return result;
}

static int write_chunk(FILE *file, const struct z_api *z, const char type[4],
                       const unsigned char *data, uint32_t size) {
    unsigned char header[8];
    unsigned char crc_input[4];
    unsigned char crc_bytes[4];
    z_ulong crc;

    write_be32(header, size);
    memcpy(header + 4, type, 4);
    if (fwrite(header, 1, sizeof(header), file) != sizeof(header))
        return -1;
    if (size != 0 && fwrite(data, 1, size, file) != size)
        return -1;

    memcpy(crc_input, type, 4);
    crc = z->crc32(0, NULL, 0);
    crc = z->crc32(crc, crc_input, sizeof(crc_input));
    if (size != 0)
        crc = z->crc32(crc, data, size);

    write_be32(crc_bytes, (uint32_t)crc);
    return fwrite(crc_bytes, 1, sizeof(crc_bytes), file) == sizeof(crc_bytes) ? 0 : -1;
}

static int write_png_rgba8(const char *path, const struct z_api *z, const struct image *image) {
    static const unsigned char signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    FILE *file = NULL;
    unsigned char ihdr[13];
    unsigned char *scanlines = NULL;
    unsigned char *compressed = NULL;
    z_ulong scanline_size;
    z_ulong compressed_size;
    int result = -1;

    if (image->width == 0 || image->height == 0)
        return -1;
    if (image->width > UINT32_MAX / 4 || image->height > UINT32_MAX / (image->width * 4 + 1))
        return -1;

    scanline_size = (z_ulong)image->height * (z_ulong)(image->width * 4 + 1);
    scanlines = malloc((size_t)scanline_size);
    compressed_size = z->compress_bound(scanline_size);
    compressed = malloc((size_t)compressed_size);
    if (scanlines == NULL || compressed == NULL)
        goto done;

    for (uint32_t y = 0; y < image->height; y++) {
        unsigned char *dst = scanlines + (size_t)y * (image->width * 4 + 1);
        const unsigned char *src = image->rgba + (size_t)y * image->width * 4;

        dst[0] = 0;
        memcpy(dst + 1, src, (size_t)image->width * 4);
    }

    if (z->compress2(compressed, &compressed_size, scanlines, scanline_size, 1) != 0)
        goto done;

    if (mkdir_p_for_file(path) != 0)
        goto done;

    file = fopen(path, "wb");
    if (file == NULL)
        goto done;

    write_be32(ihdr, image->width);
    write_be32(ihdr + 4, image->height);
    ihdr[8] = 8;
    ihdr[9] = 6;
    ihdr[10] = 0;
    ihdr[11] = 0;
    ihdr[12] = 0;

    if (fwrite(signature, 1, sizeof(signature), file) != sizeof(signature))
        goto done;
    if (write_chunk(file, z, "IHDR", ihdr, sizeof(ihdr)) != 0)
        goto done;
    if (write_chunk(file, z, "IDAT", compressed, (uint32_t)compressed_size) != 0)
        goto done;
    if (write_chunk(file, z, "IEND", NULL, 0) != 0)
        goto done;
    if (fclose(file) != 0) {
        file = NULL;
        goto done;
    }

    file = NULL;
    result = 0;

done:
    if (file != NULL)
        fclose(file);
    free(compressed);
    free(scanlines);
    return result;
}

static uint64_t fnv1a64_update(uint64_t hash, const unsigned char *data, size_t size) {
    for (size_t i = 0; i < size; i++) {
        hash ^= data[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

static uint64_t hash_rgba(const struct image *image) {
    return fnv1a64_update(1469598103934665603ULL, image->rgba,
                          (size_t)image->width * image->height * 4);
}

static uint64_t hash_bgra(const struct image *image) {
    uint64_t hash = 1469598103934665603ULL;

    for (uint32_t y = 0; y < image->height; y++) {
        for (uint32_t x = 0; x < image->width; x++) {
            const unsigned char *p = image->rgba + ((size_t)y * image->width + x) * 4;
            unsigned char bgra[4] = {p[2], p[1], p[0], p[3]};
            hash = fnv1a64_update(hash, bgra, sizeof(bgra));
        }
    }

    return hash;
}

static uint64_t hash_rgb(const struct image *image) {
    uint64_t hash = 1469598103934665603ULL;

    for (uint32_t y = 0; y < image->height; y++) {
        for (uint32_t x = 0; x < image->width; x++) {
            const unsigned char *p = image->rgba + ((size_t)y * image->width + x) * 4;
            hash = fnv1a64_update(hash, p, 3);
        }
    }

    return hash;
}

static int has_suffix(const char *value, const char *suffix) {
    size_t value_len = strlen(value);
    size_t suffix_len = strlen(suffix);

    return value_len >= suffix_len && strcmp(value + value_len - suffix_len, suffix) == 0;
}

static int path_join(char *out, size_t out_size, const char *left, const char *right) {
    int written = snprintf(out, out_size, "%s/%s", left, right);
    return written > 0 && (size_t)written < out_size;
}

static int is_detail_texture(const char *relative_path) {
    return has_suffix(relative_path, ".tga");
}

static enum texture_kind texture_kind_for_path(const char *relative_path,
                                               const struct image *image) {
    uint64_t pixels = (uint64_t)image->width * image->height;

    if (strstr(relative_path, "#normal") != NULL)
        return pixels >= 16384 ? TEXTURE_NORMAL : TEXTURE_SKIP;

    if (strstr(relative_path, "fonts/") == relative_path ||
        strstr(relative_path, "/fonts/") != NULL)
        return pixels >= 262144 ? TEXTURE_FONT : TEXTURE_SKIP;
    if (pixels < 65536)
        return TEXTURE_SKIP;

    return is_detail_texture(relative_path) ? TEXTURE_DETAIL : TEXTURE_COLOR;
}

static enum texture_kind texture_kind_for_mips_path(const char *relative_path) {
    if (strstr(relative_path, "#normal") != NULL)
        return TEXTURE_NORMAL;

    return is_detail_texture(relative_path) ? TEXTURE_DETAIL : TEXTURE_COLOR;
}

static int mkdir_p_for_file(const char *path) {
    char copy[PATH_MAX];

    if (strlen(path) >= sizeof(copy))
        return -1;

    strcpy(copy, path);
    for (char *p = copy + 1; *p != '\0'; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(copy, 0777) != 0 && errno != EEXIST)
                return -1;
            *p = '/';
        }
    }

    return 0;
}

static const char *find_astc_source(uint64_t hash, uint32_t width, uint32_t height,
                                    const char *block) {
    for (size_t i = 0; i < astc_source_count; i++) {
        const struct astc_source *source = &astc_sources[i];

        if (source->hash == hash && source->width == width && source->height == height &&
            strcmp(source->block, block) == 0) {
            return source->relative_path;
        }
    }

    return NULL;
}

static int remember_astc_source(uint64_t hash, uint32_t width, uint32_t height, const char *block,
                                const char *relative_path) {
    struct astc_source *sources;
    struct astc_source *source;

    if (strlen(block) >= sizeof(astc_sources[0].block) ||
        strlen(relative_path) >= sizeof(astc_sources[0].relative_path)) {
        return -1;
    }

    sources = realloc(astc_sources, (astc_source_count + 1) * sizeof(*astc_sources));
    if (sources == NULL)
        return -1;

    astc_sources = sources;
    source = &astc_sources[astc_source_count++];
    source->hash = hash;
    source->width = width;
    source->height = height;
    strcpy(source->block, block);
    strcpy(source->relative_path, relative_path);
    return 0;
}

static int write_entry(FILE *jobs, FILE *manifest, const char *source_path, const char *cache_dir,
                       const char *relative_path, const struct image *image,
                       enum texture_kind kind) {
    char astc_rel[PATH_MAX];
    char astc_path[PATH_MAX];
    const char *block = kind == TEXTURE_COLOR ? "8x8" : kind == TEXTURE_FONT ? "4x4" : "6x6";
    const char *profile = "-cl";
    const char *gl_format =
        kind == TEXTURE_COLOR ? "0x93B7" : kind == TEXTURE_FONT ? "0x93B0" : "0x93B4";
    const char *kind_name = kind == TEXTURE_NORMAL   ? "normal"
                            : kind == TEXTURE_FONT   ? "font"
                            : kind == TEXTURE_DETAIL ? "detail"
                                                     : "color";
    uint64_t rgba_hash = hash_rgba(image);
    uint64_t bgra_hash = hash_bgra(image);
    uint64_t rgb_hash = hash_rgb(image);
    const char *selected_astc_rel = find_astc_source(rgba_hash, image->width, image->height, block);

    if (selected_astc_rel == NULL) {
        if (snprintf(astc_rel, sizeof(astc_rel), "%s.astc", relative_path) >=
            (int)sizeof(astc_rel)) {
            return -1;
        }
        if (!path_join(astc_path, sizeof(astc_path), cache_dir, astc_rel))
            return -1;
        if (mkdir_p_for_file(astc_path) != 0)
            return -1;

        fprintf(jobs, "%s\t%s\t%s\t%s\t%s\t%u\t%u\t%s\n", source_path, astc_path, profile,
                block, kind_name, image->width, image->height, relative_path);
        if (remember_astc_source(rgba_hash, image->width, image->height, block, astc_rel) != 0)
            return -1;
        selected_astc_rel = astc_sources[astc_source_count - 1].relative_path;
    }

    /* The runtime hook only sees decoded upload bytes, so match the layouts the game may pass to
     * GL. */
    fprintf(manifest, "%016llx\t%u\t%u\t%s\t%s\t%s\t%s\t%s\n", (unsigned long long)rgba_hash,
            image->width, image->height, gl_format, selected_astc_rel, kind_name, relative_path,
            "rgba");
    fprintf(manifest, "%016llx\t%u\t%u\t%s\t%s\t%s\t%s\t%s\n", (unsigned long long)bgra_hash,
            image->width, image->height, gl_format, selected_astc_rel, kind_name, relative_path,
            "bgra");
    fprintf(manifest, "%016llx\t%u\t%u\t%s\t%s\t%s\t%s\t%s\n", (unsigned long long)rgb_hash,
            image->width, image->height, gl_format, selected_astc_rel, kind_name, relative_path,
            "rgb");
    return 0;
}

static int make_slice_image(const struct image *source, uint32_t x, uint32_t width,
                            struct image *slice) {
    memset(slice, 0, sizeof(*slice));
    if (x > source->width || width == 0 || width > source->width - x)
        return -1;

    slice->width = width;
    slice->height = source->height;
    slice->rgba = malloc((size_t)slice->width * slice->height * 4);
    if (slice->rgba == NULL)
        return -1;

    for (uint32_t y = 0; y < source->height; y++) {
        const unsigned char *src = source->rgba + ((size_t)y * source->width + x) * 4;
        unsigned char *dst = slice->rgba + (size_t)y * slice->width * 4;

        memcpy(dst, src, (size_t)slice->width * 4);
    }

    return 0;
}

static int write_slice_entries(FILE *jobs, FILE *manifest, const char *cache_dir,
                               const char *relative_path, const struct image *image,
                               enum texture_kind kind, const struct z_api *z) {
    const uint32_t slice_width = 1024;

    /* Some large textures are uploaded as fixed-width strips instead of a single full image. */
    if (image->width <= slice_width || image->width % slice_width != 0)
        return 0;

    for (uint32_t x = 0; x < image->width; x += slice_width) {
        struct image slice = {0};
        char slice_rel[PATH_MAX];
        char slice_source[PATH_MAX];

        if (snprintf(slice_rel, sizeof(slice_rel), ".generated-slices/%s.x%u.w%u.png",
                     relative_path, x, slice_width) >= (int)sizeof(slice_rel)) {
            return -1;
        }
        if (!path_join(slice_source, sizeof(slice_source), cache_dir, slice_rel))
            return -1;

        if (make_slice_image(image, x, slice_width, &slice) != 0)
            return -1;

        if (write_png_rgba8(slice_source, z, &slice) != 0 ||
            write_entry(jobs, manifest, slice_source, cache_dir, slice_rel, &slice, kind) != 0) {
            free(slice.rgba);
            return -1;
        }

        free(slice.rgba);
    }

    return 0;
}

static int write_generated_entries(FILE *jobs, FILE *manifest, const char *cache_dir,
                                   const char *generated_prefix, const char *relative_path,
                                   const struct image *image, enum texture_kind kind,
                                   const struct z_api *z) {
    char generated_rel[PATH_MAX];
    char generated_path[PATH_MAX];

    if (snprintf(generated_rel, sizeof(generated_rel), "%s/%s.png", generated_prefix,
                 relative_path) >= (int)sizeof(generated_rel)) {
        return -1;
    }
    if (!path_join(generated_path, sizeof(generated_path), cache_dir, generated_rel))
        return -1;

    if (write_png_rgba8(generated_path, z, image) != 0 ||
        write_entry(jobs, manifest, generated_path, cache_dir, generated_rel, image, kind) != 0 ||
        write_slice_entries(jobs, manifest, cache_dir, generated_rel, image, kind, z) != 0) {
        return -1;
    }

    return 0;
}

static int write_raw_mips_entries(FILE *jobs, FILE *manifest, const char *cache_dir,
                                  const char *relative_path, const char *source_path,
                                  const struct z_api *z, int *count) {
    struct buffer mips = {0};
    uint32_t width;
    uint32_t height;
    size_t offset = 17;
    int result = -1;

    if (read_file(source_path, &mips) != 0)
        return -1;
    if (mips.size < 17)
        goto done;

    /* Format 0 stores mip data as raw RGBA8. BC/DXT mips need a separate decoder. */
    if (mips.data[0] != 0) {
        result = 0;
        goto done;
    }

    width = read_le32(mips.data + 5);
    height = read_le32(mips.data + 9);
    if (width == 0 || height == 0 || width > UINT32_MAX / 4 || height > UINT32_MAX / (width * 4)) {
        goto done;
    }

    for (uint32_t level = 0; level < mips.data[1]; level++) {
        uint32_t data_size;
        struct image image;
        enum texture_kind kind;
        char level_rel[PATH_MAX];

        if (level == 0) {
            data_size = read_le32(mips.data + 13);
        } else {
            if (offset + 4 > mips.size)
                goto done;
            data_size = read_le32(mips.data + offset);
            offset += 4;
        }

        if (width > UINT32_MAX / 4 || height > UINT32_MAX / (width * 4) ||
            data_size != width * height * 4 || (size_t)data_size > mips.size - offset) {
            goto done;
        }

        memset(&image, 0, sizeof(image));
        image.width = width;
        image.height = height;
        image.rgba = mips.data + offset;

        kind = texture_kind_for_mips_path(relative_path);
        if (kind != TEXTURE_SKIP) {
            if (snprintf(level_rel, sizeof(level_rel), "%s.level%u", relative_path, level) >=
                (int)sizeof(level_rel)) {
                goto done;
            }
            if (write_generated_entries(jobs, manifest, cache_dir, ".generated-mips", level_rel,
                                        &image, kind, z) != 0) {
                goto done;
            }
            (*count)++;
        }

        offset += data_size;
        width = width > 1 ? width / 2 : 1;
        height = height > 1 ? height / 2 : 1;
    }

    result = 0;

done:
    buffer_free(&mips);
    return result;
}

static int walk_textures(const char *textures_dir, const char *cache_dir, const char *relative_dir,
                         const struct z_api *z, FILE *jobs, FILE *manifest, int *count) {
    char dir_path[PATH_MAX];
    DIR *dir;
    struct dirent *entry;

    if (!path_join(dir_path, sizeof(dir_path), textures_dir, relative_dir))
        return -1;

    dir = opendir(dir_path);
    if (dir == NULL)
        return -1;

    while ((entry = readdir(dir)) != NULL) {
        char child_rel[PATH_MAX];
        char child_path[PATH_MAX];
        struct stat st;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        if (relative_dir[0] == '\0') {
            if (snprintf(child_rel, sizeof(child_rel), "%s", entry->d_name) >=
                (int)sizeof(child_rel)) {
                closedir(dir);
                return -1;
            }
        } else if (!path_join(child_rel, sizeof(child_rel), relative_dir, entry->d_name)) {
            closedir(dir);
            return -1;
        }

        if (!path_join(child_path, sizeof(child_path), textures_dir, child_rel)) {
            closedir(dir);
            return -1;
        }

        if (lstat(child_path, &st) != 0) {
            closedir(dir);
            return -1;
        }

        if (S_ISDIR(st.st_mode)) {
            if (walk_textures(textures_dir, cache_dir, child_rel, z, jobs, manifest, count) != 0) {
                closedir(dir);
                return -1;
            }
        } else if (S_ISREG(st.st_mode) &&
                   (has_suffix(entry->d_name, ".png") || has_suffix(entry->d_name, ".tga"))) {
            struct image image = {0};
            enum texture_kind kind;

            if (has_suffix(entry->d_name, ".png")) {
                if (decode_png_rgba8(child_path, z, &image) != 0)
                    continue;
            } else if (decode_tga_rgba8(child_path, &image) != 0) {
                continue;
            }

            kind = texture_kind_for_path(child_rel, &image);
            if (kind != TEXTURE_SKIP) {
                if (write_entry(jobs, manifest, child_path, cache_dir, child_rel, &image, kind) !=
                        0 ||
                    write_slice_entries(jobs, manifest, cache_dir, child_rel, &image, kind, z) !=
                        0) {
                    free(image.rgba);
                    closedir(dir);
                    return -1;
                }
                (*count)++;
            }

            free(image.rgba);
        } else if (S_ISREG(st.st_mode) && has_suffix(entry->d_name, ".mips")) {
            if (write_raw_mips_entries(jobs, manifest, cache_dir, child_rel, child_path, z,
                                       count) != 0) {
                closedir(dir);
                return -1;
            }
        }
    }

    closedir(dir);
    return 0;
}

int main(int argc, char **argv) {
    struct z_api z;
    char data_dir[PATH_MAX];
    FILE *jobs = NULL;
    FILE *manifest = NULL;
    int count = 0;
    int result = 1;

    if (argc != 5) {
        fprintf(stderr, "usage: %s <gamedata-dir> <cache-dir> <jobs.tsv> <manifest.tsv>\n",
                argv[0]);
        return 2;
    }

    if (!path_join(data_dir, sizeof(data_dir), argv[1], "data")) {
        fprintf(stderr, "Data path is too long.\n");
        return 1;
    }

    if (load_zlib(&z) != 0) {
        fprintf(stderr, "Failed to load zlib.\n");
        return 1;
    }

    jobs = fopen(argv[3], "w");
    manifest = fopen(argv[4], "w");
    if (jobs == NULL || manifest == NULL)
        goto done;

    if (walk_textures(data_dir, argv[2], "", &z, jobs, manifest, &count) != 0)
        goto done;

    printf("Prepared ASTC jobs for %d textures.\n", count);
    result = 0;

done:
    if (jobs != NULL)
        fclose(jobs);
    if (manifest != NULL)
        fclose(manifest);
    if (z.handle != NULL)
        dlclose(z.handle);
    free(astc_sources);
    return result;
}
