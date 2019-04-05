//
// Copyright 2019 Sepehr Taghdisian (septag@github). All rights reserved.
// License: https://github.com/septag/atlasc#license-bsd-2-clause
//

#include "sx/allocator.h"
#include "sx/array.h"
#include "sx/cmdline.h"
#include "sx/io.h"
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
    int         alpha_threshold;
    float       dist_threshold;
    char**      in_filepaths;
    int         num_inputs;
    const char* out_filepath;
    int         max_width;
    int         max_height;
    int         border;
    int         pot;
    int         padding;
    int         mesh;
    int         max_verts_per_mesh;
} atlasc_args;

typedef struct {
    uint8_t* src_image;
    sx_ivec2 src_size;
    sx_irect sprite_rect;
    sx_irect sheet_rect;

    // sprite-mesh data (if set)
    uint16_t  num_tris;
    int       num_points;
    sx_ivec2* pts;
    uint16_t* tris;
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

        if (sprites[i].tris) {
            sx_free(g_alloc, sprites[i].tris);
        }

        if (sprites[i].pts) {
            sx_free(g_alloc, sprites[i].pts);
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

static void atlasc__triangulate(atlasc__sprite* spr, const s2o_point* pts, int pt_count,
                                int max_verts) {
    const float delta = 0.5f;
    const float threshold_start = 0.5f;

    float      threshold = threshold_start;
    int        num_verts;
    s2o_point* temp_pts = sx_malloc(g_alloc, sizeof(s2o_point) * pt_count);
    if (!temp_pts) {
        sx_out_of_memory();
        return;
    }

    do {
        num_verts = pt_count;
        sx_memcpy(temp_pts, pts, num_verts * sizeof(s2o_point));
        s2o_distance_based_path_simplification(temp_pts, &num_verts, threshold);
        threshold += delta;
    } while (num_verts > max_verts);
    //printf("threshold: %.2f\n", threshold);

    // triangulate
    del_point2d_t* dpts = sx_malloc(g_alloc, sizeof(del_point2d_t) * num_verts);
    if (!dpts) {
        sx_out_of_memory();
        return;
    }
    for (int i = 0; i < num_verts; i++) {
        dpts[i].x = (double)temp_pts[i].x;
        dpts[i].y = (double)temp_pts[i].y;
    }

    delaunay2d_t* polys = delaunay2d_from(dpts, num_verts);
    sx_assert(polys);
    tri_delaunay2d_t* tris = tri_delaunay2d_from(polys);
    sx_assert(tris);
    sx_free(g_alloc, dpts);
    delaunay2d_release(polys);

    sx_assert(tris->num_triangles < UINT16_MAX);
    spr->tris = sx_malloc(g_alloc, sizeof(uint16_t) * tris->num_triangles * 3);
    spr->pts = sx_malloc(g_alloc, sizeof(sx_ivec2) * tris->num_points);
    sx_assert(spr->tris);
    sx_assert(spr->pts);

    for (unsigned int i = 0; i < tris->num_triangles; i++) {
        unsigned int index = i * 3;
        spr->tris[index] = (uint16_t)tris->tris[index];
        spr->tris[index + 1] = (uint16_t)tris->tris[index + 1];
        spr->tris[index + 2] = (uint16_t)tris->tris[index + 2];
    }
    for (unsigned int i = 0; i < tris->num_points; i++) {
        spr->pts[i] = sx_ivec2i((int)tris->points[i].x, (int)tris->points[i].y);
    }
    spr->num_tris = (uint16_t)tris->num_triangles;
    spr->num_points = (int)tris->num_points;

    tri_delaunay2d_release(tris);
    sx_free(g_alloc, temp_pts);
}

static bool atlasc__save(const atlasc_args* args, const atlasc__sprite* sprites, int num_sprites,
                         const uint8_t* dst, int dst_w, int dst_h) {
    char file_ext[32];
    char basename[256];
    char image_filepath[256];
    char image_filename[256];

    sx_os_path_splitext(file_ext, sizeof(file_ext), basename, sizeof(basename), args->out_filepath);
    sx_strcpy(image_filepath, sizeof(image_filepath), basename);
    sx_strcat(image_filepath, sizeof(image_filepath), ".png");

    if (!stbi_write_png(image_filepath, dst_w, dst_h, 4, dst, dst_w * 4)) {
        printf("could not write image: %s\n", image_filepath);
    }
    sx_os_path_basename(image_filename, sizeof(image_filename), image_filepath);

    // write atlas description into json file
    sjson_context* jctx = sjson_create_context(0, 0, (void*)g_alloc);
    if (!jctx) {
        sx_assert(0);
        return false;
    }

    sjson_node* jroot = sjson_mkobject(jctx);
    sjson_put_string(jctx, jroot, "image", image_filename);
    sjson_put_int(jctx, jroot, "image_width", dst_w);
    sjson_put_int(jctx, jroot, "image_height", dst_h);

    sjson_node* jsprites = sjson_put_array(jctx, jroot, "sprites");
    char        name[256];
    for (int i = 0; i < num_sprites; i++) {
        const atlasc__sprite* spr = &sprites[i];
        sjson_node*           jsprite = sjson_mkobject(jctx);

        sx_os_path_unixpath(name, sizeof(name), args->in_filepaths[i]);
        sjson_put_string(jctx, jsprite, "name", name);
        sjson_put_ints(jctx, jsprite, "size", spr->src_size.n, 2);
        sjson_put_ints(jctx, jsprite, "sprite_rect", spr->sprite_rect.f, 4);
        sjson_put_ints(jctx, jsprite, "sheet_rect", spr->sheet_rect.f, 4);

        if (spr->num_tris) {
            sjson_node* jmesh = sjson_put_obj(jctx, jsprite, "mesh");
            sjson_put_int(jctx, jmesh, "num_tris", spr->num_tris);
            sjson_put_int(jctx, jmesh, "num_vertices", spr->num_points);
            sjson_put_int16s(jctx, jmesh, "indices", spr->tris, spr->num_tris * 3);
            sjson_node* jverts = sjson_put_array(jctx, jmesh, "vertices");
            for (int v = 0; v < spr->num_points; v++) {
                sjson_node* jvert = sjson_mkarray(jctx);
                sjson_append_element(jvert, sjson_mknumber(jctx, (double)spr->pts[v].x));
                sjson_append_element(jvert, sjson_mknumber(jctx, (double)spr->pts[v].y));
                sjson_append_element(jverts, jvert);
            }
        }

        sjson_append_element(jsprites, jsprite);
    }

    char* jout = sjson_encode(jctx, jroot);
    if (!jout)
        return false;
    sx_file_writer writer;
    if (!sx_file_open_writer(&writer, args->out_filepath, 0)) {
        printf("could not open file for writing: %s\n", args->out_filepath);
        return false;
    }
    sx_file_write_text(&writer, jout);
    sx_file_close_writer(&writer);

    sjson_free_string(jctx, jout);
    sjson_destroy_context(jctx);
    return true;
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

        sx_irect sprite_rect;
        uint8_t* alpha = s2o_rgba_to_alpha(spr->src_image, spr->src_size.x, spr->src_size.y);
        uint8_t* thresholded = s2o_alpha_to_thresholded(alpha, spr->src_size.x, spr->src_size.y,
                                                        args->alpha_threshold);
        sx_free(g_alloc, alpha);
        uint8_t* outlined =
            s2o_thresholded_to_outlined(thresholded, spr->src_size.x, spr->src_size.y);
#if 0
        char tmp_file[256];
        sx_strcpy(tmp_file, sizeof(tmp_file), args->in_filepaths[i]);
        sx_strcat(tmp_file, sizeof(tmp_file), ".bmp");
        stbi_write_bmp(tmp_file, spr->src_size.x, spr->src_size.y, 1, outlined);
#endif
        sx_free(g_alloc, thresholded);

        int        pt_count;
        s2o_point* pts =
            s2o_extract_outline_path(outlined, spr->src_size.x, spr->src_size.y, &pt_count, NULL);
        sx_free(g_alloc, outlined);

        // calculate cropped rectangle
        sprite_rect = sx_irecti(INT_MAX, INT_MAX, INT_MIN, INT_MIN);
        for (int k = 0; k < pt_count; k++) {
            sx_irect_add_point(&sprite_rect, sx_ivec2i(pts[k].x, pts[k].y));
        }

        // generate mesh if set in arguments
        if (args->mesh) {
            atlasc__triangulate(spr, pts, pt_count, args->max_verts_per_mesh);
        }

        sx_free(g_alloc, pts);
        spr->sprite_rect = sprite_rect;
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
        sx_irect rc = sprites[i].sprite_rect;
        int      rc_resize = (args->border + args->padding) * 2;
        rp_rects[i].w = (rc.xmax - rc.xmin) + rc_resize;
        rp_rects[i].h = (rc.ymax - rc.ymin) + rc_resize;
    }
    stbrp_init_target(&rp_ctx, max_width, max_height, rp_nodes, num_rp_nodes);
    sx_irect final_rect = sx_irecti(INT_MAX, INT_MAX, INT_MIN, INT_MIN);
    if (stbrp_pack_rects(&rp_ctx, rp_rects, num_sprites)) {
        for (int i = 0; i < num_sprites; i++) {
            sx_irect sheet_rect =
                sx_irectwh(rp_rects[i].x, rp_rects[i].y, rp_rects[i].w, rp_rects[i].h);

            // calculate the total size of output image
            sx_irect_add_point(&final_rect, sheet_rect.vmin);
            sx_irect_add_point(&final_rect, sheet_rect.vmax);

            // shrink back rect and set the real sheet_rect for the sprite
            sprites[i].sheet_rect =
                sx_irect_expand(sheet_rect, sx_ivec2i(-args->border, -args->border));
        }
    }

    int dst_w = final_rect.xmax - final_rect.xmin;
    int dst_h = final_rect.ymax - final_rect.ymin;
    // make output size divide by 4 by default
    dst_w = sx_align_mask(dst_w, 3);
    dst_h = sx_align_mask(dst_h, 3);

    if (args->pot) {
        dst_w = sx_nearest_pow2(dst_w);
        dst_h = sx_nearest_pow2(dst_h);
    }

    uint8_t* dst = sx_malloc(g_alloc, dst_w * dst_h * 4);
    sx_memset(dst, 0x0, dst_w * dst_h * 4);
    if (!dst) {
        sx_out_of_memory();
        return false;
    }

    for (int i = 0; i < num_sprites; i++) {
        const atlasc__sprite* spr = &sprites[i];

        // remove padding and blit from src_image to dst
        sx_irect dstrc =
            sx_irect_expand(spr->sheet_rect, sx_ivec2i(-args->padding, -args->padding));
        sx_irect srcrc = spr->sprite_rect;
        atlasc__blit(dst, dstrc.xmin, dstrc.ymin, dst_w * 4, spr->src_image, srcrc.xmin, srcrc.ymin,
                     srcrc.xmax - srcrc.xmin, srcrc.ymax - srcrc.ymin, spr->src_size.x * 4, 32);
    }

    bool r = atlasc__save(args, sprites, num_sprites, dst, dst_w, dst_h);

    sx_free(g_alloc, rp_nodes);
    sx_free(g_alloc, rp_rects);
    sx_free(g_alloc, dst);
    atlasc__free_sprites(sprites, num_sprites);

    return r;
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
    atlasc_args args = { .alpha_threshold = 20,
                         .max_width = 2048,
                         .max_height = 2048,
                         .border = 2,
                         .padding = 1,
                         .max_verts_per_mesh = 25 };

    const sx_cmdline_opt cmd_opts[] = {
        { "help", 'h', SX_CMDLINE_OPTYPE_NO_ARG, 0x0, 'h', "Print help text", 0x0 },
        { "version", 'V', SX_CMDLINE_OPTYPE_FLAG_SET, &version, 1, "Print version", 0x0 },
        { "input", 'i', SX_CMDLINE_OPTYPE_REQUIRED, 0x0, 'i', "Input image file(s)", "Filepath" },
        { "output", 'o', SX_CMDLINE_OPTYPE_REQUIRED, 0x0, 'o', "Output file", "Filepath" },
        { "max-width", 'W', SX_CMDLINE_OPTYPE_OPTIONAL, 0x0, 'W',
          "Maximum output image width (default:1024)", "Pixels" },
        { "max-height", 'H', SX_CMDLINE_OPTYPE_OPTIONAL, 0x0, 'H',
          "Maximum output image height (default:1024)", "Pixels" },
        { "border", 'B', SX_CMDLINE_OPTYPE_OPTIONAL, 0x0, 'B',
          "Border size for each sprite (default:2)", "Pixels" },
        { "pot", '2', SX_CMDLINE_OPTYPE_FLAG_SET, &args.pot, 1,
          "Make output image size power-of-two", NULL },
        { "padding", 'P', SX_CMDLINE_OPTYPE_OPTIONAL, 0x0, 'P',
          "Set padding for each sprite (default:1)", "Pixels" },
        { "mesh", 'm', SX_CMDLINE_OPTYPE_FLAG_SET, &args.mesh, 1, "Make sprite meshes", NULL },
        { "max-verts", 'M', SX_CMDLINE_OPTYPE_OPTIONAL, 0x0, 'M',
          "Set maximum vertices for each generated sprite mesh (default:25)", "Number" },
        { "alpha-threshold", 'A', SX_CMDLINE_OPTYPE_OPTIONAL, 0x0, 'A',
          "Alpha threshold for cropping (0..255)", "Number" },
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
        case 'A': args.alpha_threshold = sx_toint(arg); break;
        case 'W': args.max_width = sx_toint(arg); break;
        case 'H': args.max_height = sx_toint(arg); break;
        case 'B': args.border = sx_toint(arg); break;
        case 'P': args.padding = sx_toint(arg); break;
        case 'M': args.max_verts_per_mesh = sx_toint(arg); break;
        default:  break;
        }
    }
    // clang-format on

    if (version) {
        print_version();
        return 0;
    }

    if (!args.in_filepaths) {
        puts("must set at least one input file (-i)");
        return -1;
    }

    if (!args.out_filepath) {
        puts("must set output file (-o)");
        return -1;
    }

    for (int i = 0; i < sx_array_count(args.in_filepaths); i++) {
        if (!sx_os_path_isfile(args.in_filepaths[i])) {
            printf("Invalid file path: %s\n", args.in_filepaths[i]);
            return -1;
        }
    }

    args.num_inputs = sx_array_count(args.in_filepaths);
    bool r = atlasc_make(&args);

    sx_cmdline_destroy_context(cmd, alloc);
    sx_array_free(alloc, args.in_filepaths);

#ifdef _DEBUG
    sx_dump_leaks(NULL);
#endif
    return r ? 0 : -1;
}