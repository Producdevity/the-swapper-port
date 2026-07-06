#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define GL_EXTENSIONS 0x1F03
#define GL_NO_ERROR 0
#define GL_TEXTURE_2D 0x0DE1
#define GL_RGB 0x1907
#define GL_RGBA 0x1908
#define GL_BGRA 0x80E1
#define GL_UNSIGNED_BYTE 0x1401
#define GL_VERSION 0x1F02

typedef unsigned int GLenum;
typedef int GLint;
typedef int GLsizei;
typedef unsigned char GLubyte;
typedef void (*gl_tex_image_2d_fn)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum,
                                   const void *);
typedef void (*gl_tex_sub_image_2d_fn)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum,
                                       GLenum, const void *);
typedef void (*gl_compressed_tex_image_2d_fn)(GLenum, GLint, GLenum, GLsizei, GLsizei, GLint,
                                              GLsizei, const void *);
typedef void (*gl_tex_storage_2d_fn)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
typedef void (*gl_renderbuffer_storage_fn)(GLenum, GLenum, GLsizei, GLsizei);
typedef const GLubyte *(*gl_get_string_fn)(GLenum);
typedef GLenum (*gl_get_error_fn)(void);
typedef void *(*get_proc_address_fn)(const char *);

void glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height,
                  GLint border, GLenum format, GLenum type, const void *pixels);
void glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width,
                     GLsizei height, GLenum format, GLenum type, const void *pixels);
void glTexStorage2D(GLenum target, GLsizei levels, GLenum internalformat, GLsizei width,
                    GLsizei height);
void glRenderbufferStorage(GLenum target, GLenum internalformat, GLsizei width, GLsizei height);
void *SDL_GL_GetProcAddress(const char *proc);
void *glXGetProcAddress(const char *proc);
void *glXGetProcAddressARB(const char *proc);
void *eglGetProcAddress(const char *proc);

struct astc_entry {
    uint64_t hash;
    int width;
    int height;
    unsigned int internal_format;
    char path[PATH_MAX];
};

static struct astc_entry *entries;
static size_t entry_count;
static int initialized;
static int astc_advertised = -1;
static int astc_disabled;
static int startup_logged;
static int first_result_logged;
static char cache_dir[PATH_MAX];
static size_t candidate_uploads;
static size_t matched_uploads;
static size_t replaced_uploads;
static size_t failed_uploads;
static size_t fallback_uploads;
static size_t upload_debug_count;
static size_t candidate_miss_debug_count;
static size_t candidate_miss_dump_count;
static size_t replacement_debug_count;
static size_t large_alloc_debug_count;

static gl_tex_image_2d_fn real_gl_tex_image_2d;
static gl_tex_sub_image_2d_fn real_gl_tex_sub_image_2d;
static gl_compressed_tex_image_2d_fn real_gl_compressed_tex_image_2d;
static gl_tex_storage_2d_fn real_gl_tex_storage_2d;
static gl_renderbuffer_storage_fn real_gl_renderbuffer_storage;
static gl_get_string_fn real_gl_get_string;
static gl_get_error_fn real_gl_get_error;
static get_proc_address_fn real_sdl_gl_get_proc_address;
static get_proc_address_fn real_glx_get_proc_address;
static get_proc_address_fn real_glx_get_proc_address_arb;
static get_proc_address_fn real_egl_get_proc_address;
static void *(*real_dlsym_fn)(void *, const char *);
static size_t dlsym_hook_count;
static size_t dlsym_debug_count;
static size_t dlsym_seen_count;

static int env_enabled(const char *name);

static const char *caller_path(void *return_address) {
    Dl_info caller;

    memset(&caller, 0, sizeof(caller));
    if (dladdr(return_address, &caller) == 0 || caller.dli_fname == NULL)
        return "";

    return caller.dli_fname;
}

static int caller_is_mono_runtime(void *return_address) {
    const char *path = caller_path(return_address);

    return strcmp(path, "mono") == 0 || strstr(path, "/mono") != NULL ||
           strstr(path, "libmono") != NULL || strstr(path, "mono-sgen") != NULL;
}

static void log_dlsym_hook(const char *name, void *return_address) {
    if (!env_enabled("SWAPPER_ASTC_DEBUG"))
        return;
    if (dlsym_debug_count >= 24)
        return;

    dlsym_debug_count++;
    fprintf(stderr, "SwapperASTC: dlsym hook name=%s caller=%s\n", name,
            caller_path(return_address));
}

static int is_gl_lookup_symbol(const char *name) {
    return name != NULL &&
           (strcmp(name, "glTexImage2D") == 0 || strcmp(name, "glTexSubImage2D") == 0 ||
            strcmp(name, "glTexStorage2D") == 0 || strcmp(name, "glRenderbufferStorage") == 0 ||
            strcmp(name, "SDL_GL_GetProcAddress") == 0 || strcmp(name, "glXGetProcAddress") == 0 ||
            strcmp(name, "glXGetProcAddressARB") == 0 || strcmp(name, "eglGetProcAddress") == 0);
}

static void log_dlsym_seen(const char *name, void *return_address) {
    if (!env_enabled("SWAPPER_ASTC_DEBUG"))
        return;
    if (dlsym_seen_count >= 48 || !is_gl_lookup_symbol(name))
        return;

    dlsym_seen_count++;
    fprintf(stderr, "SwapperASTC: dlsym seen name=%s caller=%s mono=%s\n", name,
            caller_path(return_address), caller_is_mono_runtime(return_address) ? "yes" : "no");
}

static void *hooked_gl_symbol(const char *name) {
    if (strcmp(name, "glTexImage2D") == 0)
        return (void *)glTexImage2D;
    if (strcmp(name, "glTexSubImage2D") == 0)
        return (void *)glTexSubImage2D;
    if (strcmp(name, "glTexStorage2D") == 0)
        return (void *)glTexStorage2D;
    if (strcmp(name, "glRenderbufferStorage") == 0)
        return (void *)glRenderbufferStorage;
    if (strcmp(name, "SDL_GL_GetProcAddress") == 0)
        return (void *)SDL_GL_GetProcAddress;
    if (strcmp(name, "glXGetProcAddress") == 0)
        return (void *)glXGetProcAddress;
    if (strcmp(name, "glXGetProcAddressARB") == 0)
        return (void *)glXGetProcAddressARB;
    if (strcmp(name, "eglGetProcAddress") == 0)
        return (void *)eglGetProcAddress;

    return NULL;
}

static void *real_dlsym(void *handle, const char *name) {
    if (real_dlsym_fn == NULL)
        real_dlsym_fn = (void *(*)(void *, const char *))dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.17");
    if (real_dlsym_fn == NULL)
        real_dlsym_fn = (void *(*)(void *, const char *))dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.34");
    if (real_dlsym_fn == NULL)
        return NULL;

    return real_dlsym_fn(handle, name);
}

static int dlsym_hook_enabled(void) {
    const char *disabled = getenv("SWAPPER_ASTC_DISABLE_DLSYM_HOOK");

    return disabled == NULL || strcmp(disabled, "1") != 0;
}

static int env_enabled(const char *name) {
    const char *value = getenv(name);

    return value != NULL && strcmp(value, "1") == 0;
}

static uint64_t fnv1a64(const unsigned char *data, size_t size) {
    uint64_t hash = 1469598103934665603ULL;

    for (size_t i = 0; i < size; i++) {
        hash ^= data[i];
        hash *= 1099511628211ULL;
    }

    return hash;
}

static void *resolve_symbol(const char *name) {
    void *symbol = real_dlsym(RTLD_NEXT, name);

    if (symbol == NULL && real_sdl_gl_get_proc_address != NULL)
        symbol = real_sdl_gl_get_proc_address(name);
    if (symbol == NULL && real_glx_get_proc_address != NULL)
        symbol = real_glx_get_proc_address(name);
    if (symbol == NULL && real_glx_get_proc_address_arb != NULL)
        symbol = real_glx_get_proc_address_arb(name);
    if (symbol == NULL && real_egl_get_proc_address != NULL)
        symbol = real_egl_get_proc_address(name);

    return symbol;
}

static void resolve_gl(void) {
    if (real_sdl_gl_get_proc_address == NULL)
        real_sdl_gl_get_proc_address =
            (get_proc_address_fn)real_dlsym(RTLD_NEXT, "SDL_GL_GetProcAddress");
    if (real_glx_get_proc_address == NULL)
        real_glx_get_proc_address = (get_proc_address_fn)real_dlsym(RTLD_NEXT, "glXGetProcAddress");
    if (real_glx_get_proc_address_arb == NULL)
        real_glx_get_proc_address_arb =
            (get_proc_address_fn)real_dlsym(RTLD_NEXT, "glXGetProcAddressARB");
    if (real_egl_get_proc_address == NULL)
        real_egl_get_proc_address = (get_proc_address_fn)real_dlsym(RTLD_NEXT, "eglGetProcAddress");

    if (real_gl_tex_image_2d == NULL)
        real_gl_tex_image_2d = (gl_tex_image_2d_fn)resolve_symbol("glTexImage2D");
    if (real_gl_tex_sub_image_2d == NULL)
        real_gl_tex_sub_image_2d = (gl_tex_sub_image_2d_fn)resolve_symbol("glTexSubImage2D");
    if (real_gl_compressed_tex_image_2d == NULL) {
        real_gl_compressed_tex_image_2d =
            (gl_compressed_tex_image_2d_fn)resolve_symbol("glCompressedTexImage2D");
    }
    if (real_gl_tex_storage_2d == NULL)
        real_gl_tex_storage_2d = (gl_tex_storage_2d_fn)resolve_symbol("glTexStorage2D");
    if (real_gl_renderbuffer_storage == NULL) {
        real_gl_renderbuffer_storage =
            (gl_renderbuffer_storage_fn)resolve_symbol("glRenderbufferStorage");
    }
    if (real_gl_get_string == NULL)
        real_gl_get_string = (gl_get_string_fn)resolve_symbol("glGetString");
    if (real_gl_get_error == NULL)
        real_gl_get_error = (gl_get_error_fn)resolve_symbol("glGetError");
}

static int parse_hex64(const char *value, uint64_t *out) {
    char *end = NULL;
    unsigned long long parsed = strtoull(value, &end, 16);

    if (end == value || *end != '\0')
        return -1;

    *out = (uint64_t)parsed;
    return 0;
}

static int parse_u32(const char *value, unsigned int *out) {
    char *end = NULL;
    unsigned long parsed = strtoul(value, &end, 0);

    if (end == value || *end != '\0' || parsed > 0xffffffffUL)
        return -1;

    *out = (unsigned int)parsed;
    return 0;
}

static void append_entry(const struct astc_entry *entry) {
    struct astc_entry *new_entries =
        realloc(entries, (entry_count + 1) * sizeof(struct astc_entry));

    if (new_entries == NULL)
        return;

    entries = new_entries;
    entries[entry_count++] = *entry;
}

static void load_manifest(void) {
    const char *manifest_path = getenv("SWAPPER_ASTC_MANIFEST");
    const char *cache_path = getenv("SWAPPER_ASTC_CACHE_DIR");
    FILE *file;
    char line[8192];

    initialized = 1;
    if (manifest_path == NULL || manifest_path[0] == '\0' || cache_path == NULL ||
        cache_path[0] == '\0') {
        return;
    }

    snprintf(cache_dir, sizeof(cache_dir), "%s", cache_path);
    file = fopen(manifest_path, "r");
    if (file == NULL)
        return;

    while (fgets(line, sizeof(line), file) != NULL) {
        char *hash_text = strtok(line, "\t\r\n");
        char *width_text = strtok(NULL, "\t\r\n");
        char *height_text = strtok(NULL, "\t\r\n");
        char *format_text = strtok(NULL, "\t\r\n");
        char *path_text = strtok(NULL, "\t\r\n");
        struct astc_entry entry;
        unsigned int width;
        unsigned int height;

        if (hash_text == NULL || width_text == NULL || height_text == NULL || format_text == NULL ||
            path_text == NULL) {
            continue;
        }

        memset(&entry, 0, sizeof(entry));
        if (parse_hex64(hash_text, &entry.hash) != 0 || parse_u32(width_text, &width) != 0 ||
            parse_u32(height_text, &height) != 0 ||
            parse_u32(format_text, &entry.internal_format) != 0) {
            continue;
        }

        entry.width = (int)width;
        entry.height = (int)height;
        if (snprintf(entry.path, sizeof(entry.path), "%s/%s", cache_dir, path_text) >=
            (int)sizeof(entry.path)) {
            continue;
        }

        append_entry(&entry);
    }

    fclose(file);
}

static int extension_contains(const char *extensions, const char *needle) {
    size_t needle_len = strlen(needle);
    const char *p = extensions;

    while ((p = strstr(p, needle)) != NULL) {
        char before = p == extensions ? ' ' : p[-1];
        char after = p[needle_len];

        if ((before == ' ' || before == '\0') && (after == ' ' || after == '\0'))
            return 1;
        p += needle_len;
    }

    return 0;
}

static int supports_gles32(void) {
    const char *version = (const char *)real_gl_get_string(GL_VERSION);
    int major = 0;
    int minor = 0;

    if (version == NULL)
        return 0;

    if (sscanf(version, "OpenGL ES %d.%d", &major, &minor) != 2)
        return 0;

    return major > 3 || (major == 3 && minor >= 2);
}

static int astc_extension_advertised(void) {
    const char *extensions;

    if (astc_advertised >= 0)
        return astc_advertised;

    resolve_gl();
    astc_advertised = 0;
    if (real_gl_get_string == NULL)
        return 0;

    extensions = (const char *)real_gl_get_string(GL_EXTENSIONS);
    if (extensions == NULL)
        return 0;

    astc_advertised = extension_contains(extensions, "GL_KHR_texture_compression_astc_ldr") ||
                      extension_contains(extensions, "GL_KHR_texture_compression_astc_hdr") ||
                      extension_contains(extensions, "GL_OES_texture_compression_astc") ||
                      supports_gles32();
    return astc_advertised;
}

static int can_try_astc(void) {
    resolve_gl();
    if (astc_disabled || real_gl_compressed_tex_image_2d == NULL)
        return 0;

    astc_extension_advertised();
    return 1;
}

__attribute__((constructor)) static void log_loaded(void) {
    if (!env_enabled("SWAPPER_ASTC_DEBUG"))
        return;

    fprintf(stderr, "SwapperASTC: preload loaded\n");
}

static void log_startup_once(void) {
    if (startup_logged)
        return;

    startup_logged = 1;
    if (!initialized)
        load_manifest();
    astc_extension_advertised();
    if (env_enabled("SWAPPER_ASTC_DEBUG")) {
        fprintf(stderr, "SwapperASTC: entries=%zu advertised=%s compressed_upload=%s\n",
                entry_count, astc_advertised > 0 ? "yes" : "no",
                real_gl_compressed_tex_image_2d != NULL ? "yes" : "no");
    }
}

static int has_candidate_dimension(int width, int height) {
    if (!initialized)
        load_manifest();

    for (size_t i = 0; i < entry_count; i++) {
        if (entries[i].width == width && entries[i].height == height)
            return 1;
    }

    return 0;
}

static void log_upload_debug(const char *api, GLint level, GLint internalformat, GLsizei width,
                             GLsizei height, GLenum format, GLenum type, const void *pixels,
                             size_t pixels_size) {
    if (!env_enabled("SWAPPER_ASTC_DEBUG"))
        return;
    if (upload_debug_count >= 32)
        return;

    upload_debug_count++;
    fprintf(stderr,
            "SwapperASTC: upload %s level=%d width=%d height=%d internal=0x%x format=0x%x "
            "type=0x%x pixels=%s bytes=%zu candidate_dim=%s\n",
            api, level, width, height, internalformat, format, type, pixels == NULL ? "no" : "yes",
            pixels_size, has_candidate_dimension(width, height) ? "yes" : "no");
}

static void log_large_allocation(const char *api, GLsizei width, GLsizei height,
                                 GLenum internalformat, GLenum format, GLenum type,
                                 const void *pixels, size_t pixels_size) {
    uint64_t bytes;

    if (!env_enabled("SWAPPER_ASTC_DEBUG"))
        return;

    if (width <= 0 || height <= 0)
        return;

    bytes = (uint64_t)width * (uint64_t)height * 4;
    if (bytes < 1024 * 1024 || large_alloc_debug_count >= 64)
        return;

    large_alloc_debug_count++;
    fprintf(stderr,
            "SwapperASTC: large allocation %s width=%d height=%d internal=0x%x "
            "format=0x%x type=0x%x estimated_rgba_bytes=%llu pixels=%s bytes=%zu "
            "candidate_dim=%s\n",
            api, width, height, internalformat, format, type, (unsigned long long)bytes,
            pixels == NULL ? "no" : "yes", pixels_size,
            has_candidate_dimension(width, height) ? "yes" : "no");
}

static const struct astc_entry *find_entry(uint64_t hash, int width, int height) {
    if (!initialized)
        load_manifest();

    for (size_t i = 0; i < entry_count; i++) {
        if (entries[i].hash == hash && entries[i].width == width && entries[i].height == height)
            return &entries[i];
    }

    return NULL;
}

static void dump_candidate_miss(GLsizei width, GLsizei height, uint64_t hash, const void *pixels,
                                size_t pixels_size) {
    char path[PATH_MAX];
    FILE *file;

    if (!env_enabled("SWAPPER_ASTC_DUMP_MISSES") || candidate_miss_dump_count >= 8 ||
        cache_dir[0] == '\0')
        return;
    if (pixels_size < 1024 * 1024)
        return;

    if (snprintf(path, sizeof(path), "%s/miss-%zux%zu-%016llx-%zu.rgba", cache_dir, (size_t)width,
                 (size_t)height, (unsigned long long)hash,
                 candidate_miss_dump_count) >= (int)sizeof(path))
        return;

    file = fopen(path, "wb");
    if (file == NULL)
        return;

    if (pixels_size != 0)
        fwrite(pixels, 1, pixels_size, file);
    fclose(file);

    fprintf(stderr, "SwapperASTC: dumped candidate miss %s bytes=%zu\n", path, pixels_size);
    candidate_miss_dump_count++;
}

static unsigned char *read_file(const char *path, size_t *out_size) {
    FILE *file = fopen(path, "rb");
    long size;
    unsigned char *data;

    if (file == NULL)
        return NULL;

    if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) < 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }

    data = malloc((size_t)size);
    if (data == NULL && size != 0) {
        fclose(file);
        return NULL;
    }

    if (size != 0 && fread(data, 1, (size_t)size, file) != (size_t)size) {
        free(data);
        fclose(file);
        return NULL;
    }

    fclose(file);
    *out_size = (size_t)size;
    return data;
}

static int astc_payload(const unsigned char *data, size_t size, int width, int height,
                        const unsigned char **payload, size_t *payload_size) {
    unsigned int xsize;
    unsigned int ysize;

    if (size <= 16 || data[0] != 0x13 || data[1] != 0xab || data[2] != 0xa1 || data[3] != 0x5c)
        return -1;

    xsize = (unsigned int)data[7] | ((unsigned int)data[8] << 8) | ((unsigned int)data[9] << 16);
    ysize = (unsigned int)data[10] | ((unsigned int)data[11] << 8) | ((unsigned int)data[12] << 16);
    if ((int)xsize != width || (int)ysize != height)
        return -1;

    *payload = data + 16;
    *payload_size = size - 16;
    return 0;
}

static size_t pixel_size(GLenum format, GLenum type, GLsizei width, GLsizei height) {
    size_t components;

    if (type != GL_UNSIGNED_BYTE || width <= 0 || height <= 0)
        return 0;

    if (format == GL_RGBA || format == GL_BGRA)
        components = 4;
    else if (format == GL_RGB)
        components = 3;
    else
        return 0;

    return (size_t)width * (size_t)height * components;
}

static int try_astc_upload(const char *api, GLenum target, GLint level, GLsizei width,
                           GLsizei height, GLint border, const void *pixels, size_t pixels_size) {
    uint64_t hash;
    const struct astc_entry *entry;

    if (!can_try_astc() || !has_candidate_dimension(width, height))
        return 0;

    log_startup_once();
    candidate_uploads++;
    hash = fnv1a64((const unsigned char *)pixels, pixels_size);
    entry = find_entry(hash, width, height);
    if (entry == NULL) {
        if (env_enabled("SWAPPER_ASTC_DEBUG") &&
            (candidate_miss_debug_count < 32 || pixels_size >= 1024 * 1024)) {
            if (candidate_miss_debug_count < 32)
                candidate_miss_debug_count++;
            fprintf(stderr,
                    "SwapperASTC: candidate miss %s level=%d width=%d height=%d hash=%016llx "
                    "bytes=%zu\n",
                    api, level, width, height, (unsigned long long)hash, pixels_size);
        }
        dump_candidate_miss(width, height, hash, pixels, pixels_size);
        return 0;
    }

    size_t astc_size = 0;
    unsigned char *astc = read_file(entry->path, &astc_size);
    const unsigned char *payload = NULL;
    size_t payload_size = 0;

    matched_uploads++;
    if (astc != NULL &&
        astc_payload(astc, astc_size, width, height, &payload, &payload_size) == 0) {
        real_gl_compressed_tex_image_2d(target, level, entry->internal_format, width, height,
                                        border, (GLsizei)payload_size, payload);
        GLenum error = real_gl_get_error == NULL ? GL_NO_ERROR : real_gl_get_error();

        if (!first_result_logged && env_enabled("SWAPPER_ASTC_DEBUG")) {
            first_result_logged = 1;
            fprintf(stderr,
                    "SwapperASTC: first %s upload %s level=%d width=%d height=%d format=0x%x "
                    "error=0x%x\n",
                    api, error == GL_NO_ERROR ? "accepted" : "rejected", level, width, height,
                    entry->internal_format, error);
        } else if (!first_result_logged) {
            first_result_logged = 1;
        }

        if (error == GL_NO_ERROR) {
            replaced_uploads++;
            if (env_enabled("SWAPPER_ASTC_DEBUG") &&
                (replacement_debug_count < 32 || pixels_size >= 1024 * 1024)) {
                if (replacement_debug_count < 32)
                    replacement_debug_count++;
                fprintf(stderr,
                        "SwapperASTC: replacement accepted %s level=%d width=%d height=%d "
                        "hash=%016llx path=%s\n",
                        api, level, width, height, (unsigned long long)hash, entry->path);
            }
            free(astc);
            return 1;
        }

        failed_uploads++;
        if (astc_advertised <= 0) {
            /* If the driver accepts the extension poorly, fall back instead of risking broken
             * draws. */
            astc_disabled = 1;
            fprintf(stderr, "SwapperASTC: disabling ASTC after rejected unadvertised upload\n");
        }
    }

    free(astc);
    return 0;
}

void glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height,
                  GLint border, GLenum format, GLenum type, const void *pixels) {
    size_t pixels_size;

    resolve_gl();
    if (real_gl_tex_image_2d == NULL)
        return;

    pixels_size = pixel_size(format, type, width, height);
    log_upload_debug("glTexImage2D", level, internalformat, width, height, format, type, pixels,
                     pixels_size);
    log_large_allocation("glTexImage2D", width, height, internalformat, format, type, pixels,
                         pixels_size);
    if (target != GL_TEXTURE_2D || border != 0 || pixels == NULL || pixels_size == 0) {
        log_startup_once();
        real_gl_tex_image_2d(target, level, internalformat, width, height, border, format, type,
                             pixels);
        return;
    }

    if (try_astc_upload("glTexImage2D", target, level, width, height, border, pixels, pixels_size))
        return;

    fallback_uploads++;
    real_gl_tex_image_2d(target, level, internalformat, width, height, border, format, type,
                         pixels);
}

void glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width,
                     GLsizei height, GLenum format, GLenum type, const void *pixels) {
    size_t pixels_size;

    resolve_gl();
    if (real_gl_tex_sub_image_2d == NULL)
        return;

    pixels_size = pixel_size(format, type, width, height);
    log_upload_debug("glTexSubImage2D", level, 0, width, height, format, type, pixels, pixels_size);
    log_large_allocation("glTexSubImage2D", width, height, 0, format, type, pixels, pixels_size);
    if (target != GL_TEXTURE_2D || xoffset != 0 || yoffset != 0 || pixels == NULL ||
        pixels_size == 0) {
        log_startup_once();
        real_gl_tex_sub_image_2d(target, level, xoffset, yoffset, width, height, format, type,
                                 pixels);
        return;
    }

    if (try_astc_upload("glTexSubImage2D", target, level, width, height, 0, pixels, pixels_size))
        return;

    fallback_uploads++;
    real_gl_tex_sub_image_2d(target, level, xoffset, yoffset, width, height, format, type, pixels);
}

void glTexStorage2D(GLenum target, GLsizei levels, GLenum internalformat, GLsizei width,
                    GLsizei height) {
    resolve_gl();
    if (real_gl_tex_storage_2d == NULL)
        return;

    log_large_allocation("glTexStorage2D", width, height, internalformat, 0, 0, NULL, 0);
    real_gl_tex_storage_2d(target, levels, internalformat, width, height);
}

void glRenderbufferStorage(GLenum target, GLenum internalformat, GLsizei width, GLsizei height) {
    resolve_gl();
    if (real_gl_renderbuffer_storage == NULL)
        return;

    log_large_allocation("glRenderbufferStorage", width, height, internalformat, 0, 0, NULL, 0);
    real_gl_renderbuffer_storage(target, internalformat, width, height);
}

__attribute__((destructor)) static void log_summary(void) {
    if (!env_enabled("SWAPPER_ASTC_DEBUG"))
        return;
    if (!startup_logged && !initialized)
        return;

    fprintf(stderr,
            "SwapperASTC: summary entries=%zu dlsym_hooks=%zu candidates=%zu matched=%zu "
            "replaced=%zu failed=%zu fallback=%zu disabled=%s\n",
            entry_count, dlsym_hook_count, candidate_uploads, matched_uploads, replaced_uploads,
            failed_uploads, fallback_uploads, astc_disabled ? "yes" : "no");
}

void *SDL_GL_GetProcAddress(const char *proc) {
    if (real_sdl_gl_get_proc_address == NULL)
        real_sdl_gl_get_proc_address =
            (get_proc_address_fn)real_dlsym(RTLD_NEXT, "SDL_GL_GetProcAddress");
    if (proc != NULL && strcmp(proc, "glTexImage2D") == 0)
        return (void *)glTexImage2D;
    if (proc != NULL && strcmp(proc, "glTexSubImage2D") == 0)
        return (void *)glTexSubImage2D;
    if (proc != NULL && strcmp(proc, "glTexStorage2D") == 0)
        return (void *)glTexStorage2D;
    if (proc != NULL && strcmp(proc, "glRenderbufferStorage") == 0)
        return (void *)glRenderbufferStorage;
    return real_sdl_gl_get_proc_address == NULL ? NULL : real_sdl_gl_get_proc_address(proc);
}

void *glXGetProcAddress(const char *proc) {
    if (real_glx_get_proc_address == NULL)
        real_glx_get_proc_address = (get_proc_address_fn)real_dlsym(RTLD_NEXT, "glXGetProcAddress");
    if (proc != NULL && strcmp(proc, "glTexImage2D") == 0)
        return (void *)glTexImage2D;
    if (proc != NULL && strcmp(proc, "glTexSubImage2D") == 0)
        return (void *)glTexSubImage2D;
    if (proc != NULL && strcmp(proc, "glTexStorage2D") == 0)
        return (void *)glTexStorage2D;
    if (proc != NULL && strcmp(proc, "glRenderbufferStorage") == 0)
        return (void *)glRenderbufferStorage;
    return real_glx_get_proc_address == NULL ? NULL : real_glx_get_proc_address(proc);
}

void *glXGetProcAddressARB(const char *proc) {
    if (real_glx_get_proc_address_arb == NULL)
        real_glx_get_proc_address_arb =
            (get_proc_address_fn)real_dlsym(RTLD_NEXT, "glXGetProcAddressARB");
    if (proc != NULL && strcmp(proc, "glTexImage2D") == 0)
        return (void *)glTexImage2D;
    if (proc != NULL && strcmp(proc, "glTexSubImage2D") == 0)
        return (void *)glTexSubImage2D;
    if (proc != NULL && strcmp(proc, "glTexStorage2D") == 0)
        return (void *)glTexStorage2D;
    if (proc != NULL && strcmp(proc, "glRenderbufferStorage") == 0)
        return (void *)glRenderbufferStorage;
    return real_glx_get_proc_address_arb == NULL ? NULL : real_glx_get_proc_address_arb(proc);
}

void *eglGetProcAddress(const char *proc) {
    if (real_egl_get_proc_address == NULL)
        real_egl_get_proc_address = (get_proc_address_fn)real_dlsym(RTLD_NEXT, "eglGetProcAddress");
    if (proc != NULL && strcmp(proc, "glTexImage2D") == 0)
        return (void *)glTexImage2D;
    if (proc != NULL && strcmp(proc, "glTexSubImage2D") == 0)
        return (void *)glTexSubImage2D;
    if (proc != NULL && strcmp(proc, "glTexStorage2D") == 0)
        return (void *)glTexStorage2D;
    if (proc != NULL && strcmp(proc, "glRenderbufferStorage") == 0)
        return (void *)glRenderbufferStorage;
    return real_egl_get_proc_address == NULL ? NULL : real_egl_get_proc_address(proc);
}

void *dlsym(void *handle, const char *name) {
    void *return_address = __builtin_return_address(0);
    void *hooked;

    log_dlsym_seen(name, return_address);

    if (!dlsym_hook_enabled())
        return real_dlsym(handle, name);

    if (!caller_is_mono_runtime(return_address))
        return real_dlsym(handle, name);

    /* The managed GL wrapper can resolve entry points through dlsym, bypassing GetProcAddress
     * hooks. */
    hooked = hooked_gl_symbol(name);
    if (hooked != NULL) {
        dlsym_hook_count++;
        log_dlsym_hook(name, return_address);
        return hooked;
    }

    return real_dlsym(handle, name);
}
