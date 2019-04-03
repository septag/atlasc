#include "sx/allocator.h"
#include "sx/array.h"
#include "sx/cmdline.h"
#include "sx/math.h"
#include "sx/os.h"
#include "sx/string.h"

#include "delaunay/delaunay.h"

#include <stdio.h>

static const sx_alloc* g_alloc;

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_MALLOC(sz) sx_malloc(g_alloc, sz)
#define STBI_REALLOC(p, newsz) sx_realloc(g_alloc, p, newsz)
#define STBI_FREE(p) sx_free(g_alloc, p)
#include "stb/stb_image.h"

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_MSC_SECURE_CRT
#define STBIW_MALLOC(sz) sx_malloc(g_alloc, sz)
#define STBIW_REALLOC(p, newsz) sx_realloc(g_alloc, p, newsz)
#define STBIW_FREE(p) sx_free(g_alloc, p)
#include "stb/stb_image_write.h"

#define STB_RECT_PACK_IMPLEMENTATION
#define STBRP_ASSERT sx_assert
#define STBRP_STATIC
#include "stb/stb_rect_pack.h"

#define SJSON_IMPLEMENTATION
#define sjson_malloc(user, size) sx_malloc((const sx_alloc*)user, size)
#define sjson_free(user, ptr) sx_free((const sx_alloc*)user, ptr);
#define sjson_realloc(user, ptr, size) sx_realloc((const sx_alloc*)user, ptr, size);
#define sjson_assert(_e) sx_assert(_e);
#define sjson_snprintf sx_snprintf
#include "sjson/sjson.h"

#define S2O_IMPLEMENTATION
#define S2O_STATIC
#define S2O_MALLOC(sz) sx_malloc(g_alloc, sz)
#include "sproutline/sproutline.h"

#define VERSION 1000

typedef struct atlasc_args {
    int         threshold;
    char**      in_filepaths;
    int         num_inputs;
    const char* out_filepath;
    int         max_width;
    int         max_height;
} atlasc_args;

typedef struct {
    uint8_t* src_image;
    sx_ivec2 src_size;
    sx_irect cropped_rect;
    sx_irect sheet_rect;
    sx_rect  sheet_uv;
} atlasc__sprite;

static char g_error_str[512];

static void print_version() {
    printf("atlasc v%d.%d.%d\n", VERSION / 1000, (VERSION % 1000) / 10, (VERSION % 10));
    puts("http://www.github.com/septag/atlasc");
}

static void print_help(sx_cmdline_context* ctx) {
    char buffer[4096];
    print_version();
    puts("");
    puts(sx_cmdline_create_help_string(ctx, buffer, sizeof(buffer)));
}

static void atlasc__free_sprites(atlasc__sprite* sprites, int num_sprites) {
    for (int i = 0; i < num_sprites; i++) {
        if (sprites[i].src_image) {
            stbi_image_free(sprites[i].src_image);
        }
    }
    sx_free(g_alloc, sprites);
}

static void atlasc__blit(uint8_t* dst, int dst_x, int dst_y, int dst_pitch, const uint8_t* src,
                         int src_x, int src_y, int src_w, int src_h, int src_pitch, int bpp) {
    sx_assert(dst);
    sx_assert(src);

    const int      pixel_sz = bpp / 8;
    const uint8_t* src_ptr = src + src_y * src_pitch + src_x * pixel_sz;
    uint8_t*       dst_ptr = dst + dst_y * dst_pitch + dst_x * pixel_sz;
    for (int y = src_y; y < (src_y + src_h); y++) {
        sx_memcpy(dst_ptr, src_ptr, src_w * pixel_sz);
        src_ptr += src_pitch;
        dst_ptr += dst_pitch;
    }
}

static bool atlasc_make(const atlasc_args* args) {
    sx_assert(args);

    int             num_sprites = args->num_inputs;
    atlasc__sprite* sprites = sx_malloc(g_alloc, sizeof(atlasc__sprite) * num_sprites);
    if (!sprites) {
        sx_out_of_memory();
        return false;
    }
    sx_memset(sprites, 0x0, sizeof(atlasc__sprite) * num_sprites);

    for (int i = 0; i < num_sprites; i++) {
        atlasc__sprite* spr = &sprites[i];
        int             comp;
        if (!sx_os_path_isfile(args->in_filepaths[i])) {
            sx_snprintf(g_error_str, sizeof(g_error_str), "input image not found: %s",
                        args->in_filepaths[i]);
            atlasc__free_sprites(sprites, num_sprites);
            return false;
        }
        stbi_uc* pixels =
            stbi_load(args->in_filepaths[i], &spr->src_size.x, &spr->src_size.y, &comp, 4);
        if (!pixels) {
            sx_snprintf(g_error_str, sizeof(g_error_str), "invalid image format: %s",
                        args->in_filepaths[i]);
            atlasc__free_sprites(sprites, num_sprites);
            return false;
        }
        spr->src_image = pixels;

        uint8_t* alpha = s2o_rgba_to_alpha(spr->src_image, spr->src_size.x, spr->src_size.y);
        uint8_t* thresholded =
            s2o_alpha_to_thresholded(alpha, spr->src_size.x, spr->src_size.y, args->threshold);
        sx_free(g_alloc, alpha);
        uint8_t* outlined =
            s2o_thresholded_to_outlined(thresholded, spr->src_size.x, spr->src_size.y);
        char tmp_file[256];
        sx_strcpy(tmp_file, sizeof(tmp_file), args->in_filepaths[i]);
        sx_strcat(tmp_file, sizeof(tmp_file), ".bmp");
        //stbi_write_bmp(tmp_file, spr->src_size.x, spr->src_size.y, 1, outlined);
        sx_free(g_alloc, thresholded);

        int        pt_count;
        s2o_point* pts =
            s2o_extract_outline_path(outlined, spr->src_size.x, spr->src_size.y, &pt_count, NULL);
        sx_free(g_alloc, outlined);

        // calculate cropped rectangle
        sx_irect cropped = { .xmin = INT_MAX, .ymin = INT_MAX, .xmax = INT_MIN, .ymax = INT_MIN };
        for (int k = 0; k < pt_count; k++) {
            sx_irect_add_point(&cropped, sx_ivec2i(pts[k].x, pts[k].y));
        }
        spr->cropped_rect = cropped;
        sx_free(g_alloc, pts);
    }

    // pack sprites into a sheet
    stbrp_context rp_ctx;
    int           max_width = args->max_width;
    int           max_height = args->max_height;
    int           num_rp_nodes = max_width + max_height;
    stbrp_rect*   rp_rects = sx_malloc(g_alloc, num_sprites * sizeof(stbrp_rect));
    stbrp_node*   rp_nodes = sx_malloc(g_alloc, num_rp_nodes * sizeof(stbrp_node));
    if (!rp_rects || !rp_nodes) {
        sx_out_of_memory();
        return false;
    }
    sx_memset(rp_rects, 0x0, sizeof(stbrp_rect) * num_sprites);

    for (int i = 0; i < num_sprites; i++) {
        sx_irect rc = sprites[i].cropped_rect;
        rp_rects[i].w = rc.xmax - rc.xmin;
        rp_rects[i].h = rc.ymax - rc.ymin;
    }
    stbrp_init_target(&rp_ctx, max_width, max_height, rp_nodes, num_rp_nodes);
    sx_irect final_rect = sx_irecti(INT_MAX, INT_MAX, INT_MIN, INT_MIN);
    if (stbrp_pack_rects(&rp_ctx, rp_rects, num_sprites)) {
        for (int i = 0; i < num_sprites; i++) {
            sprites[i].sheet_rect =
                sx_irectwh(rp_rects[i].x, rp_rects[i].y, rp_rects[i].w, rp_rects[i].h);
            printf("%s: %d %d %d %d\n", args->in_filepaths[i], sprites[i].sheet_rect.xmin,
                   sprites[i].sheet_rect.ymin, 
                   sprites[i].sheet_rect.xmax - sprites[i].sheet_rect.xmin,
                   sprites[i].sheet_rect.ymax - sprites[i].sheet_rect.ymin);
            sx_irect_add_point(&final_rect, sprites[i].sheet_rect.vmin);
            sx_irect_add_point(&final_rect, sprites[i].sheet_rect.vmax);
        }
    }

    printf("final: %d %d %d %d\n", final_rect.xmin, final_rect.ymin, final_rect.xmax,
           final_rect.ymax);
    int      dst_w = final_rect.xmax - final_rect.xmin;
    int      dst_h = final_rect.ymax - final_rect.ymin;
    //int      dst_w = 1024;
    //int      dst_h = 1024;
    uint8_t* dst = sx_malloc(g_alloc, dst_w * dst_h * 4);
    sx_memset(dst, 0x0, dst_w * dst_h * 4);
    if (!dst) {
        sx_out_of_memory();
        return false;
    }

    for (int i = 0; i < num_sprites; i++) {
        const atlasc__sprite* spr = &sprites[i];
        sx_irect              dstrc = spr->sheet_rect;
        sx_irect              srcrc = spr->cropped_rect;
        atlasc__blit(dst, dstrc.xmin, dstrc.ymin, dst_w * 4, spr->src_image, srcrc.xmin, srcrc.ymin,
                     srcrc.xmax - srcrc.xmin, srcrc.ymax - srcrc.ymin, spr->src_size.x * 4, 32);
    }

    stbi_write_png(args->out_filepath, dst_w, dst_h, 4, dst, dst_w * 4);

    sx_free(g_alloc, rp_nodes);
    sx_free(g_alloc, rp_rects);
    atlasc__free_sprites(sprites, num_sprites);

    return true;
}

static const char* atlasc_error_string() {
    return g_error_str;
}

int main(int argc, char* argv[]) {
#ifdef _DEBUG
    const sx_alloc* alloc = sx_alloc_malloc_leak_detect();
#else
    const sx_alloc* alloc = sx_alloc_malloc();
#endif
    g_alloc = alloc;

    int         version = 0;
    atlasc_args args = { .threshold = 1, .max_width = 1024, .max_height = 1024 };

    const sx_cmdline_opt cmd_opts[] = {
        { "help", 'h', SX_CMDLINE_OPTYPE_NO_ARG, 0x0, 'h', "Print help text", 0x0 },
        { "version", 'V', SX_CMDLINE_OPTYPE_FLAG_SET, &version, 1, "Print version", 0x0 },
        { "input", 'i', SX_CMDLINE_OPTYPE_REQUIRED, 0x0, 'i', "Input image file(s)", "Filepath" },
        { "threshold", 'T', SX_CMDLINE_OPTYPE_OPTIONAL, 0x0, 'T', "Alpha threshold (0..255)",
          "Number" },
        { "output", 'o', SX_CMDLINE_OPTYPE_REQUIRED, 0x0, 'o', "Output file", "Filepath" },
        { "max-width", 'W', SX_CMDLINE_OPTYPE_OPTIONAL, 0x0, 1024, "Maximum output image width",
          "Pixels" },
        { "max-height", 'H', SX_CMDLINE_OPTYPE_OPTIONAL, 0x0, 1024, "Maximum output image height",
          "Pixels" },
        SX_CMDLINE_OPT_END
    };

    sx_cmdline_context* cmd = sx_cmdline_create_context(alloc, argc, argv, cmd_opts);
    sx_assert(cmd);

    int         opt;
    const char* arg;

    // clang-format off
    while ((opt = sx_cmdline_next(cmd, NULL, &arg)) != -1) {
        switch (opt) {
        case '+': printf("Argument without flag: %s\n", arg); break;
        case '?': printf("Unknown argument: %s\n", arg); exit(-1); break;
        case 'h': print_help(cmd); return 0;
        case '!': printf("Invalid use of argument: %s\n", arg); exit(-1); break;
        case 'i': sx_array_push(alloc, args.in_filepaths, (char*)arg); break;
        case 'o': args.out_filepath = arg; break;
        case 'T': args.threshold = sx_toint(arg); break;
        case 'W': args.max_width = sx_toint(arg); break;
        case 'H': args.max_height = sx_toint(arg); break;
        default:  break;
        }
    }
    // clang-format on

    if (version) {
        print_version();
        return 0;
    }

    for (int i = 0; i < sx_array_count(args.in_filepaths); i++) {
        if (!sx_os_path_isfile(args.in_filepaths[i])) {
            printf("Invalid file path: %s\n", args.in_filepaths[i]);
            return -1;
        }
    }
    args.num_inputs = sx_array_count(args.in_filepaths);
    atlasc_make(&args);

    sx_cmdline_destroy_context(cmd, alloc);
    sx_array_free(alloc, args.in_filepaths);

#ifdef _DEBUG
    sx_dump_leaks(NULL);
#endif
    return 0;
}