#include "sx/allocator.h"
#include "sx/array.h"
#include "sx/cmdline.h"
#include "sx/os.h"
#include "sx/string.h"

#include "delaunay/delaunay.h"

#include <stdio.h>

#define S2O_IMPLEMENTATION
#define S2O_STATIC
#include "sproutline/sproutline.h"

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#include "stb/stb_image.h"

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

#define SJSON_IMPLEMENTATION
#define sjson_malloc(user, size) sx_malloc((const sx_alloc*)user, size)
#define sjson_free(user, ptr) sx_free((const sx_alloc*)user, ptr);
#define sjson_realloc(user, ptr, size) sx_realloc((const sx_alloc*)user, ptr, size);
#define sjson_assert(_e) sx_assert(_e);
#define sjson_snprintf sx_snprintf
#include "sjson/sjson.h"

static const sx_alloc* g_alloc;

#define VERSION 1000

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

int main(int argc, char* argv[]) {
#ifdef _DEBUG
    const sx_alloc* alloc = sx_alloc_malloc_leak_detect();
#else
    const sx_alloc* alloc = sx_alloc_malloc();
#endif
    g_alloc = alloc;

    int         version = 0;
    int         threshold = 1;
    char**      in_filepaths = NULL;
    const char* out_filepath = NULL;

    const sx_cmdline_opt cmd_opts[] = {
        { "help", 'h', SX_CMDLINE_OPTYPE_NO_ARG, 0x0, 'h', "Print help text", 0x0 },
        { "version", 'V', SX_CMDLINE_OPTYPE_FLAG_SET, &version, 1, "Print version", 0x0 },
        { "input", 'i', SX_CMDLINE_OPTYPE_REQUIRED, 0x0, 'i', "Input image file(s)", "Filepath" },
        { "threshold", 'T', SX_CMDLINE_OPTYPE_OPTIONAL, 0x0, 'T', "Alpha threshold (0..255)",
          "Number" },
        { "output", 'o', SX_CMDLINE_OPTYPE_REQUIRED, 0x0, 'o', "Output file", "Filepath" },
        SX_CMDLINE_OPT_END
    };

    sx_cmdline_context* cmd = sx_cmdline_create_context(alloc, argc, argv, cmd_opts);
    sx_assert(cmd);

    int         opt;
    const char* arg;
    while ((opt = sx_cmdline_next(cmd, NULL, &arg)) != -1) {
        switch (opt) {
        case '+':
            printf("Argument without flag: %s\n", arg);
            break;
        case '?':
            printf("Unknown argument: %s\n", arg);
            exit(-1);
            break;
        case 'h':
            print_help(cmd);
            return 0;
        case '!':
            printf("Invalid use of argument: %s\n", arg);
            exit(-1);
            break;
        case 'i':
            sx_array_push(alloc, in_filepaths, (char*)arg);
            break;
        case 'o':
            out_filepath = arg;
            break;
        case 'T':
            threshold = sx_toint(arg);
            break;
        default:
            break;
        }
    }

    if (version) {
        print_version();
        return 0;
    }

    for (int i = 0; i < sx_array_count(in_filepaths); i++) {
        if (!sx_os_path_isfile(in_filepaths[i])) {
            printf("Invalid file path: %s\n", in_filepaths[i]);
            return -1;
        }
        puts(in_filepaths[i]);
    }

    sx_cmdline_destroy_context(cmd, alloc);
    sx_array_free(alloc, in_filepaths);

#ifdef _DEBUG
    sx_dump_leaks(NULL);
#endif
    return 0;
}