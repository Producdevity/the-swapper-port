#include <dlfcn.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

typedef unsigned long z_ulong;
typedef int (*z_uncompress_fn)(unsigned char *, z_ulong *, const unsigned char *, z_ulong);
typedef int (*z_compress2_fn)(unsigned char *, z_ulong *, const unsigned char *, z_ulong, int);
typedef z_ulong (*z_compress_bound_fn)(z_ulong);
typedef z_ulong (*z_crc32_fn)(z_ulong, const unsigned char *, unsigned int);

struct z_api
{
    void *handle;
    z_uncompress_fn uncompress;
    z_compress2_fn compress2;
    z_compress_bound_fn compress_bound;
    z_crc32_fn crc32;
};

struct buffer
{
    unsigned char *data;
    size_t size;
    size_t capacity;
};

struct image
{
    uint32_t width;
    uint32_t height;
    unsigned char *rgba;
};

static uint32_t read_be32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void write_be32(unsigned char *p, uint32_t value)
{
    p[0] = (unsigned char)(value >> 24);
    p[1] = (unsigned char)(value >> 16);
    p[2] = (unsigned char)(value >> 8);
    p[3] = (unsigned char)value;
}

static void write_le32(unsigned char *p, uint32_t value)
{
    p[0] = (unsigned char)value;
    p[1] = (unsigned char)(value >> 8);
    p[2] = (unsigned char)(value >> 16);
    p[3] = (unsigned char)(value >> 24);
}

static int buffer_reserve(struct buffer *buffer, size_t extra)
{
    size_t needed = buffer->size + extra;
    size_t capacity = buffer->capacity == 0 ? 4096 : buffer->capacity;
    unsigned char *new_data;

    if (needed < buffer->size)
        return -1;

    while (capacity < needed)
    {
        if (capacity > SIZE_MAX / 2)
            return -1;
        capacity *= 2;
    }

    if (capacity == buffer->capacity)
        return 0;

    new_data = realloc(buffer->data, capacity);
    if (new_data == NULL)
        return -1;

    buffer->data = new_data;
    buffer->capacity = capacity;
    return 0;
}

static int buffer_append(struct buffer *buffer, const void *data, size_t size)
{
    if (buffer_reserve(buffer, size) != 0)
        return -1;

    memcpy(buffer->data + buffer->size, data, size);
    buffer->size += size;
    return 0;
}

static void buffer_free(struct buffer *buffer)
{
    free(buffer->data);
    buffer->data = NULL;
    buffer->size = 0;
    buffer->capacity = 0;
}

static int read_file(const char *path, struct buffer *buffer)
{
    FILE *file = fopen(path, "rb");
    long size;

    if (file == NULL)
        return -1;

    if (fseek(file, 0, SEEK_END) != 0)
    {
        fclose(file);
        return -1;
    }

    size = ftell(file);
    if (size < 0)
    {
        fclose(file);
        return -1;
    }

    if (fseek(file, 0, SEEK_SET) != 0)
    {
        fclose(file);
        return -1;
    }

    buffer->data = malloc((size_t)size);
    if (buffer->data == NULL && size != 0)
    {
        fclose(file);
        return -1;
    }

    buffer->size = (size_t)size;
    buffer->capacity = (size_t)size;

    if (size != 0 && fread(buffer->data, 1, (size_t)size, file) != (size_t)size)
    {
        fclose(file);
        buffer_free(buffer);
        return -1;
    }

    return fclose(file);
}

static int copy_file(const char *input_path, const char *output_path)
{
    struct buffer input = {0};
    FILE *output;
    int result = -1;

    if (strcmp(input_path, output_path) == 0)
        return 0;

    if (read_file(input_path, &input) != 0)
        return -1;

    output = fopen(output_path, "wb");
    if (output == NULL)
        goto done;

    if (input.size == 0 || fwrite(input.data, 1, input.size, output) == input.size)
        result = 0;

    if (fclose(output) != 0)
        result = -1;

done:
    buffer_free(&input);
    return result;
}

static int load_zlib(struct z_api *z)
{
    memset(z, 0, sizeof(*z));

    z->handle = dlopen("libz.so.1", RTLD_LAZY);
    if (z->handle == NULL)
        z->handle = dlopen("libz.so", RTLD_LAZY);
    if (z->handle == NULL)
        z->handle = dlopen("libz.dylib", RTLD_LAZY);
    if (z->handle == NULL)
        return -1;

    z->uncompress = (z_uncompress_fn)dlsym(z->handle, "uncompress");
    z->compress2 = (z_compress2_fn)dlsym(z->handle, "compress2");
    z->compress_bound = (z_compress_bound_fn)dlsym(z->handle, "compressBound");
    z->crc32 = (z_crc32_fn)dlsym(z->handle, "crc32");

    if (z->uncompress == NULL || z->compress2 == NULL || z->compress_bound == NULL || z->crc32 == NULL)
        return -1;

    return 0;
}

static int paeth(int a, int b, int c)
{
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

static int decode_png_rgba8(const char *path, const struct z_api *z, struct image *image)
{
    static const unsigned char signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    struct buffer png = {0};
    struct buffer idat = {0};
    unsigned char *scanlines = NULL;
    z_ulong scanline_size;
    size_t offset = 8;
    uint32_t width = 0;
    uint32_t height = 0;
    int result = -1;

    memset(image, 0, sizeof(*image));

    if (read_file(path, &png) != 0)
        return -1;

    if (png.size < sizeof(signature) || memcmp(png.data, signature, sizeof(signature)) != 0)
        goto done;

    while (offset + 12 <= png.size)
    {
        uint32_t length = read_be32(png.data + offset);
        const unsigned char *type = png.data + offset + 4;
        const unsigned char *chunk = png.data + offset + 8;

        offset += 8;
        if (length > png.size - offset - 4)
            goto done;

        if (memcmp(type, "IHDR", 4) == 0)
        {
            if (length != 13)
                goto done;

            width = read_be32(chunk);
            height = read_be32(chunk + 4);
            if (width == 0 || height == 0 || chunk[8] != 8 || chunk[9] != 6 || chunk[10] != 0 || chunk[11] != 0 || chunk[12] != 0)
                goto done;
        }
        else if (memcmp(type, "IDAT", 4) == 0)
        {
            if (buffer_append(&idat, chunk, length) != 0)
                goto done;
        }
        else if (memcmp(type, "IEND", 4) == 0)
        {
            break;
        }

        offset += (size_t)length + 4;
    }

    if (width == 0 || height == 0 || idat.size == 0)
        goto done;

    if (width > (UINT32_MAX / 4) || height > (UINT32_MAX / (width * 4 + 1)))
        goto done;

    scanline_size = (z_ulong)height * (z_ulong)(width * 4 + 1);
    scanlines = malloc((size_t)scanline_size);
    image->rgba = malloc((size_t)width * (size_t)height * 4);
    if (scanlines == NULL || image->rgba == NULL)
        goto done;

    if (z->uncompress(scanlines, &scanline_size, idat.data, (z_ulong)idat.size) != 0)
        goto done;

    if (scanline_size != (z_ulong)height * (z_ulong)(width * 4 + 1))
        goto done;

    for (uint32_t y = 0; y < height; y++)
    {
        const unsigned char *src = scanlines + (size_t)y * (width * 4 + 1);
        unsigned char *dst = image->rgba + (size_t)y * width * 4;
        const unsigned char *prev = y == 0 ? NULL : image->rgba + (size_t)(y - 1) * width * 4;
        int filter = src[0];

        src++;
        if (filter < 0 || filter > 4)
            goto done;

        for (uint32_t x = 0; x < width * 4; x++)
        {
            int left = x >= 4 ? dst[x - 4] : 0;
            int up = prev == NULL ? 0 : prev[x];
            int up_left = (prev != NULL && x >= 4) ? prev[x - 4] : 0;
            int value = src[x];

            switch (filter)
            {
                case 0:
                    break;
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
            }

            dst[x] = (unsigned char)(value & 0xff);
        }
    }

    image->width = width;
    image->height = height;
    result = 0;

done:
    if (result != 0)
    {
        free(image->rgba);
        memset(image, 0, sizeof(*image));
    }
    free(scanlines);
    buffer_free(&idat);
    buffer_free(&png);
    return result;
}

static unsigned char encode_component(double value)
{
    int out = (int)(value * 255.0 + 0.5);

    if (out < 0)
        return 0;
    if (out > 255)
        return 255;
    return (unsigned char)out;
}

static struct image resample_normal_map(const struct image *input, uint32_t out_width, uint32_t out_height)
{
    struct image output = {0};

    output.rgba = malloc((size_t)out_width * out_height * 4);
    if (output.rgba == NULL)
        return output;

    output.width = out_width;
    output.height = out_height;

    for (uint32_t y = 0; y < out_height; y++)
    {
        uint32_t y0 = (uint32_t)(((uint64_t)y * input->height) / out_height);
        uint32_t y1 = (uint32_t)(((uint64_t)(y + 1) * input->height) / out_height);

        if (y1 <= y0)
            y1 = y0 + 1;

        for (uint32_t x = 0; x < out_width; x++)
        {
            uint32_t x0 = (uint32_t)(((uint64_t)x * input->width) / out_width);
            uint32_t x1 = (uint32_t)(((uint64_t)(x + 1) * input->width) / out_width);
            double nx = 0.0;
            double ny = 0.0;
            double nz = 0.0;
            uint32_t count = 0;

            if (x1 <= x0)
                x1 = x0 + 1;

            for (uint32_t sy = y0; sy < y1; sy++)
            {
                for (uint32_t sx = x0; sx < x1; sx++)
                {
                    const unsigned char *p = input->rgba + ((size_t)sy * input->width + sx) * 4;

                    /* The Swapper stores normal maps in DXT5nm layout: X in alpha, Y in green, Z in blue. */
                    nx += (double)p[3] / 127.5 - 1.0;
                    ny += (double)p[1] / 127.5 - 1.0;
                    nz += (double)p[2] / 127.5 - 1.0;
                    count++;
                }
            }

            if (count != 0)
            {
                double length;
                unsigned char *p = output.rgba + ((size_t)y * out_width + x) * 4;

                nx /= count;
                ny /= count;
                nz /= count;
                length = sqrt(nx * nx + ny * ny + nz * nz);
                if (length < 0.000001)
                {
                    nx = 0.0;
                    ny = 0.0;
                    nz = 1.0;
                }
                else
                {
                    nx /= length;
                    ny /= length;
                    nz /= length;
                }

                p[0] = 0;
                p[1] = encode_component(ny * 0.5 + 0.5);
                p[2] = encode_component(nz * 0.5 + 0.5);
                p[3] = encode_component(nx * 0.5 + 0.5);
            }
        }
    }

    return output;
}

static struct image downscale_normal_map(const struct image *input, uint32_t max_dimension)
{
    uint32_t out_width;
    uint32_t out_height;

    if (input->width <= max_dimension && input->height <= max_dimension)
        return (struct image){0};

    if (input->width >= input->height)
    {
        out_width = max_dimension;
        out_height = (uint32_t)(((uint64_t)input->height * max_dimension + input->width / 2) / input->width);
    }
    else
    {
        out_height = max_dimension;
        out_width = (uint32_t)(((uint64_t)input->width * max_dimension + input->height / 2) / input->height);
    }

    if (out_width == 0)
        out_width = 1;
    if (out_height == 0)
        out_height = 1;

    return resample_normal_map(input, out_width, out_height);
}

static int write_chunk(struct buffer *png, const struct z_api *z, const char type[4], const unsigned char *data, uint32_t size)
{
    unsigned char header[8];
    uint32_t crc;
    unsigned char crc_bytes[4];

    write_be32(header, size);
    memcpy(header + 4, type, 4);

    if (buffer_append(png, header, sizeof(header)) != 0)
        return -1;
    if (size != 0 && buffer_append(png, data, size) != 0)
        return -1;

    crc = (uint32_t)z->crc32(0, NULL, 0);
    crc = (uint32_t)z->crc32(crc, (const unsigned char *)type, 4);
    if (size != 0)
        crc = (uint32_t)z->crc32(crc, data, size);
    write_be32(crc_bytes, crc);

    return buffer_append(png, crc_bytes, sizeof(crc_bytes));
}

static int encode_png_rgba8(const char *path, const struct z_api *z, const struct image *image)
{
    static const unsigned char signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    struct buffer raw = {0};
    struct buffer compressed = {0};
    struct buffer png = {0};
    unsigned char ihdr[13];
    z_ulong compressed_size;
    char temp_path[4096];
    FILE *file = NULL;
    int result = -1;

    if (snprintf(temp_path, sizeof(temp_path), "%s.tmp.%ld", path, (long)getpid()) >= (int)sizeof(temp_path))
        return -1;

    raw.size = (size_t)image->height * (image->width * 4 + 1);
    raw.capacity = raw.size;
    raw.data = malloc(raw.size);
    if (raw.data == NULL)
        goto done;

    for (uint32_t y = 0; y < image->height; y++)
    {
        unsigned char *row = raw.data + (size_t)y * (image->width * 4 + 1);
        row[0] = 0;
        memcpy(row + 1, image->rgba + (size_t)y * image->width * 4, (size_t)image->width * 4);
    }

    compressed_size = z->compress_bound((z_ulong)raw.size);
    compressed.data = malloc((size_t)compressed_size);
    if (compressed.data == NULL)
        goto done;
    compressed.capacity = (size_t)compressed_size;

    if (z->compress2(compressed.data, &compressed_size, raw.data, (z_ulong)raw.size, 6) != 0)
        goto done;
    compressed.size = (size_t)compressed_size;

    write_be32(ihdr, image->width);
    write_be32(ihdr + 4, image->height);
    ihdr[8] = 8;
    ihdr[9] = 6;
    ihdr[10] = 0;
    ihdr[11] = 0;
    ihdr[12] = 0;

    if (buffer_append(&png, signature, sizeof(signature)) != 0 ||
        write_chunk(&png, z, "IHDR", ihdr, sizeof(ihdr)) != 0 ||
        write_chunk(&png, z, "IDAT", compressed.data, (uint32_t)compressed.size) != 0 ||
        write_chunk(&png, z, "IEND", NULL, 0) != 0)
    {
        goto done;
    }

    file = fopen(temp_path, "wb");
    if (file == NULL)
        goto done;

    if (fwrite(png.data, 1, png.size, file) != png.size)
        goto done;

    if (fclose(file) != 0)
    {
        file = NULL;
        goto done;
    }
    file = NULL;

    if (rename(temp_path, path) != 0)
        goto done;

    result = 0;

done:
    if (file != NULL)
        fclose(file);
    if (result != 0)
        unlink(temp_path);
    buffer_free(&raw);
    buffer_free(&compressed);
    buffer_free(&png);
    return result;
}

static int path_join(char *out, size_t out_size, const char *left, const char *right)
{
    int written = snprintf(out, out_size, "%s/%s", left, right);
    return written > 0 && (size_t)written < out_size;
}

static int has_suffix(const char *value, const char *suffix)
{
    size_t value_len = strlen(value);
    size_t suffix_len = strlen(suffix);

    return value_len >= suffix_len && strcmp(value + value_len - suffix_len, suffix) == 0;
}

static int is_normal_png(const char *name)
{
    return has_suffix(name, ".png") && strstr(name, "#normal") != NULL;
}

static int mips_path_for_png(const char *png_path, char *mips_path, size_t mips_path_size)
{
    size_t len = strlen(png_path);

    if (len < 4 || !has_suffix(png_path, ".png") || len + 2 > mips_path_size)
        return -1;

    memcpy(mips_path, png_path, len - 4);
    memcpy(mips_path + len - 4, ".mips", 6);
    return 0;
}

static int clamp_int(int value, int min_value, int max_value)
{
    if (value < min_value)
        return min_value;
    if (value > max_value)
        return max_value;
    return value;
}

static uint16_t rgb_to_565(int r, int g, int b)
{
    uint16_t out = 0;

    out |= (uint16_t)((clamp_int(r, 0, 255) * 31 + 127) / 255) << 11;
    out |= (uint16_t)((clamp_int(g, 0, 255) * 63 + 127) / 255) << 5;
    out |= (uint16_t)((clamp_int(b, 0, 255) * 31 + 127) / 255);
    return out;
}

static void rgb_from_565(uint16_t value, int *r, int *g, int *b)
{
    int r5 = (value >> 11) & 31;
    int g6 = (value >> 5) & 63;
    int b5 = value & 31;

    *r = (r5 << 3) | (r5 >> 2);
    *g = (g6 << 2) | (g6 >> 4);
    *b = (b5 << 3) | (b5 >> 2);
}

static unsigned char nearest_alpha_index(unsigned char value, const unsigned char palette[8])
{
    int best_index = 0;
    int best_distance = 256;

    for (int i = 0; i < 8; i++)
    {
        int distance = abs((int)value - (int)palette[i]);
        if (distance < best_distance)
        {
            best_distance = distance;
            best_index = i;
        }
    }

    return (unsigned char)best_index;
}

static void encode_bc3_alpha(const unsigned char alpha[16], unsigned char out[8])
{
    unsigned char min_alpha = 255;
    unsigned char max_alpha = 0;
    unsigned char palette[8];
    uint64_t bits = 0;

    for (int i = 0; i < 16; i++)
    {
        if (alpha[i] < min_alpha)
            min_alpha = alpha[i];
        if (alpha[i] > max_alpha)
            max_alpha = alpha[i];
    }

    out[0] = max_alpha;
    out[1] = min_alpha;

    palette[0] = max_alpha;
    palette[1] = min_alpha;
    if (max_alpha > min_alpha)
    {
        palette[2] = (unsigned char)((6 * max_alpha + 1 * min_alpha + 3) / 7);
        palette[3] = (unsigned char)((5 * max_alpha + 2 * min_alpha + 3) / 7);
        palette[4] = (unsigned char)((4 * max_alpha + 3 * min_alpha + 3) / 7);
        palette[5] = (unsigned char)((3 * max_alpha + 4 * min_alpha + 3) / 7);
        palette[6] = (unsigned char)((2 * max_alpha + 5 * min_alpha + 3) / 7);
        palette[7] = (unsigned char)((1 * max_alpha + 6 * min_alpha + 3) / 7);
    }
    else
    {
        palette[2] = (unsigned char)((4 * max_alpha + 1 * min_alpha + 2) / 5);
        palette[3] = (unsigned char)((3 * max_alpha + 2 * min_alpha + 2) / 5);
        palette[4] = (unsigned char)((2 * max_alpha + 3 * min_alpha + 2) / 5);
        palette[5] = (unsigned char)((1 * max_alpha + 4 * min_alpha + 2) / 5);
        palette[6] = 0;
        palette[7] = 255;
    }

    for (int i = 0; i < 16; i++)
        bits |= (uint64_t)nearest_alpha_index(alpha[i], palette) << (3 * i);

    for (int i = 0; i < 6; i++)
        out[2 + i] = (unsigned char)(bits >> (8 * i));
}

static int color_distance_sq(const int a[3], const int b[3])
{
    int dr = a[0] - b[0];
    int dg = a[1] - b[1];
    int db = a[2] - b[2];

    return dr * dr + dg * dg + db * db;
}

static void encode_bc3_color(const unsigned char rgba[64], unsigned char out[8])
{
    int min_rgb[3] = {255, 255, 255};
    int max_rgb[3] = {0, 0, 0};
    int palette[4][3];
    uint16_t color0;
    uint16_t color1;
    uint32_t bits = 0;

    for (int i = 0; i < 16; i++)
    {
        const unsigned char *p = rgba + i * 4;

        for (int c = 0; c < 3; c++)
        {
            if (p[c] < min_rgb[c])
                min_rgb[c] = p[c];
            if (p[c] > max_rgb[c])
                max_rgb[c] = p[c];
        }
    }

    color0 = rgb_to_565(max_rgb[0], max_rgb[1], max_rgb[2]);
    color1 = rgb_to_565(min_rgb[0], min_rgb[1], min_rgb[2]);
    if (color0 <= color1)
    {
        uint16_t tmp = color0;
        color0 = color1;
        color1 = tmp;
    }

    rgb_from_565(color0, &palette[0][0], &palette[0][1], &palette[0][2]);
    rgb_from_565(color1, &palette[1][0], &palette[1][1], &palette[1][2]);
    for (int c = 0; c < 3; c++)
    {
        palette[2][c] = (2 * palette[0][c] + palette[1][c] + 1) / 3;
        palette[3][c] = (palette[0][c] + 2 * palette[1][c] + 1) / 3;
    }

    for (int i = 0; i < 16; i++)
    {
        int pixel[3] = {rgba[i * 4], rgba[i * 4 + 1], rgba[i * 4 + 2]};
        int best_index = 0;
        int best_distance = color_distance_sq(pixel, palette[0]);

        for (int j = 1; j < 4; j++)
        {
            int distance = color_distance_sq(pixel, palette[j]);
            if (distance < best_distance)
            {
                best_distance = distance;
                best_index = j;
            }
        }

        bits |= (uint32_t)best_index << (2 * i);
    }

    out[0] = (unsigned char)color0;
    out[1] = (unsigned char)(color0 >> 8);
    out[2] = (unsigned char)color1;
    out[3] = (unsigned char)(color1 >> 8);
    out[4] = (unsigned char)bits;
    out[5] = (unsigned char)(bits >> 8);
    out[6] = (unsigned char)(bits >> 16);
    out[7] = (unsigned char)(bits >> 24);
}

static int write_bc3_level(FILE *file, const struct image *image)
{
    uint32_t block_width = (image->width + 3) / 4;
    uint32_t block_height = (image->height + 3) / 4;

    for (uint32_t by = 0; by < block_height; by++)
    {
        for (uint32_t bx = 0; bx < block_width; bx++)
        {
            unsigned char alpha[16];
            unsigned char rgba[64];
            unsigned char block[16];

            for (uint32_t py = 0; py < 4; py++)
            {
                for (uint32_t px = 0; px < 4; px++)
                {
                    uint32_t src_x = bx * 4 + px;
                    uint32_t src_y = by * 4 + py;
                    const unsigned char *src;
                    unsigned char *dst = rgba + (py * 4 + px) * 4;

                    if (src_x >= image->width)
                        src_x = image->width - 1;
                    if (src_y >= image->height)
                        src_y = image->height - 1;

                    src = image->rgba + ((size_t)src_y * image->width + src_x) * 4;
                    memcpy(dst, src, 4);
                    alpha[py * 4 + px] = src[3];
                }
            }

            encode_bc3_alpha(alpha, block);
            encode_bc3_color(rgba, block + 8);
            if (fwrite(block, 1, sizeof(block), file) != sizeof(block))
                return -1;
        }
    }

    return 0;
}

static uint32_t bc3_level_size(uint32_t width, uint32_t height)
{
    return ((width + 3) / 4) * ((height + 3) / 4) * 16;
}

static uint8_t mip_count_for_image(uint32_t width, uint32_t height)
{
    uint8_t count = 1;

    while (width > 1 || height > 1)
    {
        width = width > 1 ? width / 2 : 1;
        height = height > 1 ? height / 2 : 1;
        count++;
    }

    return count;
}

static int write_mips_file(const char *path, const struct image *base_image)
{
    char temp_path[PATH_MAX];
    FILE *file = NULL;
    struct image current = {0};
    int owns_current = 0;
    uint8_t mip_count;
    unsigned char header[17] = {0};
    int result = -1;

    if (snprintf(temp_path, sizeof(temp_path), "%s.tmp.%ld", path, (long)getpid()) >= (int)sizeof(temp_path))
        return -1;

    file = fopen(temp_path, "wb");
    if (file == NULL)
        return -1;

    mip_count = mip_count_for_image(base_image->width, base_image->height);
    header[0] = 3;
    header[1] = mip_count;
    write_le32(header + 4, base_image->width << 8);
    write_le32(header + 8, base_image->height << 8);
    write_le32(header + 12, base_image->width * base_image->height * 256u);

    if (fwrite(header, 1, sizeof(header), file) != sizeof(header))
        goto done;

    current = *base_image;
    for (uint8_t level = 0; level < mip_count; level++)
    {
        if (level != 0)
        {
            unsigned char size_bytes[4];
            write_le32(size_bytes, bc3_level_size(current.width, current.height));
            if (fwrite(size_bytes, 1, sizeof(size_bytes), file) != sizeof(size_bytes))
                goto done;
        }

        if (write_bc3_level(file, &current) != 0)
            goto done;

        if (level + 1 < mip_count)
        {
            struct image next = resample_normal_map(&current, current.width > 1 ? current.width / 2 : 1, current.height > 1 ? current.height / 2 : 1);
            if (next.rgba == NULL)
                goto done;
            if (owns_current)
                free(current.rgba);
            current = next;
            owns_current = 1;
        }
    }

    if (fclose(file) != 0)
    {
        file = NULL;
        goto done;
    }
    file = NULL;

    if (rename(temp_path, path) != 0)
        goto done;

    result = 0;

done:
    if (file != NULL)
        fclose(file);
    if (result != 0)
        unlink(temp_path);
    if (owns_current)
        free(current.rgba);
    return result;
}

static int process_normal_png(const char *path, const struct z_api *z, uint32_t max_dimension, int *patched_count)
{
    char mips_path[PATH_MAX];
    struct image input = {0};
    struct image output = {0};
    const struct image *patched_image;
    int result = -1;

    if (decode_png_rgba8(path, z, &input) != 0)
    {
        fprintf(stderr, "Failed to decode RGBA PNG: %s\n", path);
        return -1;
    }

    output = downscale_normal_map(&input, max_dimension);
    if (output.rgba == NULL)
    {
        result = 0;
        goto done;
    }

    if (mips_path_for_png(path, mips_path, sizeof(mips_path)) != 0)
    {
        fprintf(stderr, "Failed to build mip path for %s\n", path);
        goto done;
    }

    if (encode_png_rgba8(path, z, &output) != 0)
    {
        fprintf(stderr, "Failed to write downscaled PNG: %s\n", path);
        goto done;
    }

    patched_image = &output;
    if (write_mips_file(mips_path, patched_image) != 0)
    {
        fprintf(stderr, "Failed to write downscaled mip chain: %s\n", mips_path);
        goto done;
    }

    printf("Downscaled %s from %ux%u to %ux%u\n", path, input.width, input.height, output.width, output.height);
    (*patched_count)++;
    result = 0;

done:
    free(input.rgba);
    free(output.rgba);
    return result;
}

static int walk_textures(const char *dir_path, const struct z_api *z, uint32_t max_dimension, int *patched_count)
{
    DIR *dir = opendir(dir_path);
    struct dirent *entry;

    if (dir == NULL)
        return -1;

    while ((entry = readdir(dir)) != NULL)
    {
        char child[PATH_MAX];
        struct stat st;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        if (!path_join(child, sizeof(child), dir_path, entry->d_name))
        {
            closedir(dir);
            return -1;
        }

        if (lstat(child, &st) != 0)
        {
            closedir(dir);
            return -1;
        }

        if (S_ISDIR(st.st_mode))
        {
            if (walk_textures(child, z, max_dimension, patched_count) != 0)
            {
                closedir(dir);
                return -1;
            }
        }
        else if (S_ISREG(st.st_mode) && is_normal_png(entry->d_name))
        {
            if (process_normal_png(child, z, max_dimension, patched_count) != 0)
            {
                closedir(dir);
                return -1;
            }
        }
    }

    closedir(dir);
    return 0;
}

static int patch_normal_map_tree(const char *gamedata_dir, const struct z_api *z, uint32_t max_dimension)
{
    char textures_dir[PATH_MAX];
    int patched_count = 0;

    if (!path_join(textures_dir, sizeof(textures_dir), gamedata_dir, "data/textures"))
    {
        fprintf(stderr, "Path is too long.\n");
        return -1;
    }

    if (walk_textures(textures_dir, z, max_dimension, &patched_count) != 0)
    {
        fprintf(stderr, "Failed to downscale normal maps under %s\n", textures_dir);
        return -1;
    }

    printf("Downscaled %d normal maps to max %u pixels.\n", patched_count, max_dimension);
    return 0;
}

static int parse_max_dimension(const char *value, uint32_t *out)
{
    char *end = NULL;
    unsigned long parsed;

    errno = 0;
    parsed = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed == 0 || parsed > 8192)
        return -1;

    *out = (uint32_t)parsed;
    return 0;
}

int main(int argc, char **argv)
{
    struct z_api z;
    struct image input = {0};
    struct image output = {0};
    uint32_t max_dimension;
    int result = 1;

    if (argc != 3 && argc != 4)
    {
        fprintf(stderr, "usage: %s <gamedata-dir> <max-dimension>\n", argv[0]);
        fprintf(stderr, "       %s <input.png> <output.png> <max-dimension>\n", argv[0]);
        return 2;
    }

    if (parse_max_dimension(argv[argc - 1], &max_dimension) != 0)
    {
        fprintf(stderr, "Invalid max dimension: %s\n", argv[argc - 1]);
        return 2;
    }

    if (load_zlib(&z) != 0)
    {
        fprintf(stderr, "Failed to load zlib\n");
        return 1;
    }

    if (argc == 3)
    {
        result = patch_normal_map_tree(argv[1], &z, max_dimension) == 0 ? 0 : 1;
        goto done;
    }

    if (decode_png_rgba8(argv[1], &z, &input) != 0)
    {
        fprintf(stderr, "Failed to decode RGBA PNG: %s\n", argv[1]);
        goto done;
    }

    output = downscale_normal_map(&input, max_dimension);
    if (output.rgba == NULL)
    {
        result = copy_file(argv[1], argv[2]);
        goto done;
    }

    if (encode_png_rgba8(argv[2], &z, &output) != 0)
    {
        fprintf(stderr, "Failed to write downscaled PNG: %s\n", argv[2]);
        goto done;
    }

    if (strcmp(argv[1], argv[2]) == 0)
    {
        char mips_path[PATH_MAX];
        if (mips_path_for_png(argv[2], mips_path, sizeof(mips_path)) != 0 ||
            write_mips_file(mips_path, &output) != 0)
        {
            fprintf(stderr, "Failed to write downscaled mip chain for %s\n", argv[2]);
            goto done;
        }
    }

    result = 0;

done:
    free(input.rgba);
    free(output.rgba);
    if (z.handle != NULL)
        dlclose(z.handle);
    return result;
}
