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
    sx_ivec2* uvs;
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

        if (sprites[i].uvs) {
            sx_free(g_alloc, sprites[i].uvs);
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

static inline sx_vec2 atlasc__itof2(const s2o_point p) {
    return sx_vec2f((float)p.x, (float)p.y);
}

// modified version of:
// https://github.com/anael-seghezzi/Maratis-Tiny-C-library/blob/master/include/m_raster.h
static bool atlasc__test_line(const uint8_t* buffer, int w, int h, s2o_point p0, s2o_point p1) {
    const uint8_t* data = buffer;

    int x0 = p0.x;
    int y0 = p0.y;
    int x1 = p1.x;
    int y1 = p1.y;
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, e2;

    while (1) {
        if (x0 > -1 && y0 > -1 && x0 < w && y0 < h) {
            const uint8_t* pixel = data + (y0 * w + x0);
            if (*pixel)
                return true;    // line intersects with image data
        }

        if (x0 == x1 && y0 == y1)
            break;

        e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }

    return false;
}

// returns true if 'pts' buffer is changed
static bool atlasc__offset_pt(s2o_point* pts, int num_pts, int pt_idx, float amount, int w, int h) {
    s2o_point ipt = pts[pt_idx];
    s2o_point _ipt = ipt;
    sx_vec2   pt = atlasc__itof2(ipt);
    sx_vec2   prev_pt =
        (pt_idx > 0) ? atlasc__itof2(pts[pt_idx - 1]) : atlasc__itof2(pts[num_pts - 1]);
    sx_vec2 next_pt =
        (pt_idx + 1) < num_pts ? atlasc__itof2(pts[pt_idx + 1]) : atlasc__itof2(pts[0]);
    sx_vec2 edge1 = sx_vec2_norm(sx_vec2_sub(prev_pt, pt));
    sx_vec2 edge2 = sx_vec2_norm(sx_vec2_sub(next_pt, pt));

    // calculate normal vector to move the point away from the polygon
    sx_vec2 n;
    sx_vec3 c = sx_vec3_cross(sx_vec3f(edge1.x, edge1.y, 0), sx_vec3f(edge2.x, edge2.y, 0));
    if (sx_equal(c.z, 0.0f, 0.00001f)) {
        n = sx_vec2_mulf(sx_vec2f(-edge1.y, edge1.x), amount);
    } else {
        // c.z < 0 -> point intersecting convex edges
        // c.z > 0 -> point intersecting concave edges
        float k = c.z < 0.0f ? -1.0f : 1.0f;
        n = sx_vec2_mulf(sx_vec2_norm(sx_vec2_add(edge1, edge2)), k * amount);
    }

    pt = sx_vec2_add(pt, n);
    ipt.x = (int)pt.x;
    ipt.y = (int)pt.y;
    ipt.x = sx_clamp(ipt.x, 0, w);
    ipt.y = sx_clamp(ipt.y, 0, h);
    pts[pt_idx] = ipt;
    return (_ipt.x != ipt.x) || (_ipt.y != ipt.y);
}

static void atlasc__fix_outline_pts(const uint8_t* thresholded, int tw, int th, s2o_point* pts,
                                    int num_pts) {
    // NOTE: winding is assumed to be CW
    const float offset_amount = 2.0f;

    for (int i = 0; i < num_pts; i++) {
        s2o_point pt = pts[i];
        int next_i = (i + 1) < num_pts ? (i + 1) : 0;

        sx_assert(!thresholded[pt.y * tw + pt.x]);    // point shouldn't be inside threshold

        s2o_point next_pt = pts[next_i];
        while (atlasc__test_line(thresholded, tw, th, pt, next_pt)) {
            if (!atlasc__offset_pt(pts, num_pts, i, offset_amount, tw, th))
                break;
            atlasc__offset_pt(pts, num_pts, next_i, offset_amount, tw, th);
            // refresh points for the new line intersection test
            pt = pts[i];
            next_pt = pts[next_i];
        }
    }
}

static void atlasc__make_mesh(atlasc__sprite* spr, const s2o_point* pts, int pt_count,
                              int max_verts, const uint8_t* thresholded, int width, int height) {
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
    // printf("threshold: %.2f\n", threshold);

    // fix any collisions with the actual image
    atlasc__fix_outline_pts(thresholded, width, height, temp_pts, num_verts);    

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
            sjson_node* jverts = sjson_put_array(jctx, jmesh, "positions");
            for (int v = 0; v < spr->num_points; v++) {
                sjson_node* jvert = sjson_mkarray(jctx);
                sjson_append_element(jvert, sjson_mknumber(jctx, (double)spr->pts[v].x));
                sjson_append_element(jvert, sjson_mknumber(jctx, (double)spr->pts[v].y));
                sjson_append_element(jverts, jvert);
            }

            sjson_node* juvs = sjson_put_array(jctx, jmesh, "uvs");
            for (int u = 0; u < spr->num_points; u++) {
                sjson_node* juv = sjson_mkarray(jctx);
                sjson_append_element(juv, sjson_mknumber(jctx, (double)spr->uvs[u].x));
                sjson_append_element(juv, sjson_mknumber(jctx, (double)spr->uvs[u].y));
                sjson_append_element(juvs, juv);
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

        uint8_t* dialate_thres =
            s2o_dilate_thresholded(thresholded, spr->src_size.x, spr->src_size.y);

        uint8_t* outlined =
            s2o_thresholded_to_outlined(dialate_thres, spr->src_size.x, spr->src_size.y);
        sx_free(g_alloc, dialate_thres);

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
            atlasc__make_mesh(spr, pts, pt_count, args->max_verts_per_mesh, thresholded,
                              spr->src_size.x, spr->src_size.y);
        }

        sx_free(g_alloc, pts);
        sx_free(g_alloc, thresholded);
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
            atlasc__sprite* spr = &sprites[i];
            sx_irect        sheet_rect =
                sx_irectwh(rp_rects[i].x, rp_rects[i].y, rp_rects[i].w, rp_rects[i].h);

            // calculate the total size of output image
            sx_irect_add_point(&final_rect, sheet_rect.vmin);
            sx_irect_add_point(&final_rect, sheet_rect.vmax);

            // shrink back rect and set the real sheet_rect for the sprite
            spr->sheet_rect = sx_irect_expand(sheet_rect, sx_ivec2i(-args->border, -args->border));
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

    // calculate UVs for sprite meshes
    if (args->mesh) {
        for (int i = 0; i < num_sprites; i++) {
            atlasc__sprite* spr = &sprites[i];
            // if sprite has mesh, calculate UVs for it
            if (spr->pts && spr->num_points) {
                const int padding = args->padding;
                sx_ivec2  offset = spr->sprite_rect.vmin;
                sx_ivec2  sheet_pos =
                    sx_ivec2i(spr->sheet_rect.xmin + padding, spr->sheet_rect.ymin + padding);
                sx_ivec2* uvs = sx_malloc(g_alloc, sizeof(sx_ivec2) * spr->num_points);
                sx_assert(uvs);
                for (int pi = 0; pi < spr->num_points; pi++) {
                    sx_ivec2 pt = spr->pts[pi];
                    uvs[pi] = sx_ivec2_add(sx_ivec2_sub(pt, offset), sheet_pos);
                }

                spr->uvs = uvs;
            }    // generate uvs
        }
    }

    for (int i = 0; i < num_sprites; i++) {
        const atlasc__sprite* spr = &sprites[i];

        // calculate UVs for sprite-meshes

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