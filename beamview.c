#include <SDL2/SDL.h>
#include <X11/Xlib.h>
#include <cairo.h>
#include <errno.h>
#include <glib.h>
#include <limits.h>
#include <math.h>
#include <poppler.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define _drop_(x) __attribute__((cleanup(drop_##x)))

#define DEFINE_DROP_FUNC(type, func)                                           \
    static inline void drop_##func(type *p) {                                  \
        if (*p)                                                                \
            func(*p);                                                          \
    }

#define DEFINE_DROP_FUNC_COERCE(type, func)                                    \
    static inline void drop_##func(void *p) {                                  \
        type *pp = (type *)p;                                                  \
        if (*pp) {                                                             \
            func((type) * pp);                                                 \
        }                                                                      \
    }

DEFINE_DROP_FUNC(cairo_surface_t *, cairo_surface_destroy)
DEFINE_DROP_FUNC(cairo_t *, cairo_destroy)
DEFINE_DROP_FUNC_COERCE(GObject *, g_object_unref)
DEFINE_DROP_FUNC_COERCE(gpointer, g_free)
DEFINE_DROP_FUNC(GError *, g_error_free)

#define expect(x)                                                              \
    do {                                                                       \
        if (!(x)) {                                                            \
            fprintf(stderr, "!(%s) at %s:%s:%d\n", #x, __FILE__, __func__,     \
                    __LINE__);                                                 \
            abort();                                                           \
        }                                                                      \
    } while (0)

#define NUM_CONTEXTS 2

struct texture_data {
    SDL_Texture *texture;
    int natural_width, natural_height;
};

struct sdl_ctx {
    SDL_Window *window;
    SDL_Renderer *renderer;
    struct texture_data texture;
    int is_fullscreen;
};

struct render_cache_entry {
    cairo_surface_t *cairo_surface;
    int img_width, img_height;
    double page_width, page_height;
};

struct prog_state {
    struct sdl_ctx *ctx;
    double init_pdf_width, init_pdf_height, current_scale;
    PopplerDocument *document;
    int num_ctx, current_page, num_pages, next_cache_index, needs_redraw;
    struct render_cache_entry **cache_entries;
};

static void toggle_fullscreen(struct sdl_ctx *ctx) {
    SDL_SetWindowFullscreen(
        ctx->window, ctx->is_fullscreen ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
    ctx->is_fullscreen = !ctx->is_fullscreen;
}

static void present_texture(SDL_Renderer *renderer, SDL_Texture *texture,
                            int natural_width, int natural_height) {
    expect(texture);

    int win_width, win_height;
    SDL_GetRendererOutputSize(renderer, &win_width, &win_height);

    float scale = fmin((float)win_width / natural_width,
                       (float)win_height / natural_height);
    int new_width = (int)(natural_width * scale);
    int new_height = (int)(natural_height * scale);

    SDL_Rect dst = {(win_width - new_width) / 2, (win_height - new_height) / 2,
                    new_width, new_height};

    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, NULL, &dst);
    SDL_RenderPresent(renderer);
}

static double compute_scale(struct sdl_ctx ctx[], int num_ctx,
                            double page_width, double page_height) {
    expect(page_width > 0 && page_height > 0);
    double scale = 0;
    for (int i = 0; i < num_ctx; i++) {
        int win_width, win_height;
        SDL_GetWindowSize(ctx[i].window, &win_width, &win_height);
        double scale_i = fmax((double)win_width / (page_width / num_ctx),
                              (double)win_height / page_height);
        scale = fmax(scale, scale_i);
    }
    return scale;
}

static cairo_surface_t *
render_page_to_cairo_surface(PopplerPage *page, double scale, int *img_width,
                             int *img_height, double *page_width,
                             double *page_height) {
    poppler_page_get_size(page, page_width, page_height);
    *img_width = (int)(*page_width * scale);
    *img_height = (int)(*page_height * scale);

    cairo_surface_t *surface = cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32, *img_width, *img_height);
    _drop_(cairo_destroy) cairo_t *cr = cairo_create(surface);
    expect(cairo_status(cr) == CAIRO_STATUS_SUCCESS);

    cairo_set_antialias(cr, CAIRO_ANTIALIAS_BEST);
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_paint(cr);

    cairo_scale(cr, scale, scale);
    poppler_page_render(page, cr);

    cairo_pattern_t *pattern = cairo_get_source(cr);
    cairo_pattern_set_filter(pattern, CAIRO_FILTER_BEST);
    cairo_surface_flush(surface);

    return surface;
}

static void free_cache_entry(struct render_cache_entry *entry) {
    expect(entry);
    cairo_surface_destroy(entry->cairo_surface);
    free(entry);
}

static struct render_cache_entry *create_cache_entry(int page_index,
                                                     struct prog_state *state) {
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    _drop_(g_object_unref) PopplerPage *page =
        poppler_document_get_page(state->document, page_index);
    expect(page);

    int img_width, img_height;
    double page_width, page_height;
    cairo_surface_t *cairo_surface =
        render_page_to_cairo_surface(page, state->current_scale, &img_width,
                                     &img_height, &page_width, &page_height);

    struct render_cache_entry *entry = calloc(1, sizeof(*entry));
    expect(entry);
    entry->page_width = page_width;
    entry->page_height = page_height;
    entry->img_width = img_width;
    entry->img_height = img_height;
    entry->cairo_surface = cairo_surface;

    clock_gettime(CLOCK_MONOTONIC, &end);
    double render_time_ms = (end.tv_sec - start.tv_sec) * 1000.0 +
                            (end.tv_nsec - start.tv_nsec) / 1000000.0;

    fprintf(stderr,
            "Cache page %d created in %.2f ms. Rendered size: %d x %d\n",
            page_index, render_time_ms, img_width, img_height);
    return entry;
}

static void free_all_cache_entries(struct render_cache_entry **cache_entries,
                                   int num_pages) {
    for (int i = 0; i < num_pages; i++) {
        if (cache_entries[i]) {
            free_cache_entry(cache_entries[i]);
            cache_entries[i] = NULL;
        }
    }
}

static int cache_complete(struct prog_state *state) {
    return state->next_cache_index == state->num_pages;
}

static void cache_one_slide(struct render_cache_entry **cache_entries,
                            int num_pages, struct prog_state *state) {
    if (cache_complete(state))
        return;

    int i = state->next_cache_index;
    while (i < num_pages && cache_entries[i])
        i++;
    if (i < num_pages) {
        cache_entries[i] = create_cache_entry(i, state);
        state->next_cache_index = i + 1;
    }
}

static int init_prog_state(struct prog_state *state, const char *pdf_file) {
    memset(state, 0, sizeof(*state));

    char resolved_path[PATH_MAX];
    if (!realpath(pdf_file, resolved_path)) {
        perror("realpath");
        return -errno;
    }
    _drop_(g_free) char *uri = g_strdup_printf("file://%s", resolved_path);
    _drop_(g_error_free) GError *error = NULL;
    state->document = poppler_document_new_from_file(uri, NULL, &error);

    if (!state->document) {
        fprintf(stderr, "Error opening PDF: %s\n", error->message);
        return -EIO;
    }

    int num_pages = poppler_document_get_n_pages(state->document);
    if (num_pages <= 0) {
        fprintf(stderr, "PDF has no pages.\n");
        g_object_unref(state->document);
        return -EINVAL;
    }
    state->num_pages = num_pages;

    _drop_(g_object_unref) PopplerPage *first_page =
        poppler_document_get_page(state->document, 0);
    if (!first_page) {
        fprintf(stderr, "Failed to load first page.\n");
        g_object_unref(state->document);
        return -ENOENT;
    }
    poppler_page_get_size(first_page, &state->init_pdf_width,
                          &state->init_pdf_height);
    state->current_scale = 1.0;

    return 0;
}

static int accel_x11_error_handler(Display *dpy, XErrorEvent *event) {
    (void)dpy;
    (void)event;
    return 0;
}

static SDL_Renderer *create_renderer_with_fallback(SDL_Window *window) {
    static int use_software_only = 0;
    if (use_software_only) {
        // We already know hardware accel fails, use software right away.
        return SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    }

    // We need this to avoid a BadValue crash by default
    int (*old_handler)(Display *, XErrorEvent *) =
        XSetErrorHandler(accel_x11_error_handler);
    SDL_Renderer *renderer = SDL_CreateRenderer(
        window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    XSetErrorHandler(old_handler);
    if (!renderer) {
        fprintf(
            stderr,
            "Warning: hardware acceleration seems unavailable, using software rendering\n");
        use_software_only = 1;
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    }
    return renderer;
}

static void create_contexts(struct sdl_ctx ctx[], int num_ctx) {
    SDL_Rect display_bounds;
    expect(SDL_GetDisplayBounds(0, &display_bounds) == 0);
    const int win_width = 1280;
    const int win_height = 720;
    int center_x = display_bounds.x + (display_bounds.w - win_width) / 2;
    int center_y = display_bounds.y + (display_bounds.h - win_height) / 2;
    const int offset = 100;

    for (int i = 0; i < num_ctx; i++) {
        char title[32];
        snprintf(title, sizeof(title), "beamview: Context %d", i);
        int x = center_x + i * offset;
        int y = center_y + i * offset;
        ctx[i].window =
            SDL_CreateWindow(title, x, y, win_width, win_height,
                             SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
        expect(ctx[i].window);
        ctx[i].renderer = create_renderer_with_fallback(ctx[i].window);
        expect(ctx[i].renderer);
    }
}

static void update_scale(struct prog_state *state) {
    double page_width, page_height;
    if (state->cache_entries[state->current_page]) {
        page_width = state->cache_entries[state->current_page]->page_width;
        page_height = state->cache_entries[state->current_page]->page_height;
    } else {
        _drop_(g_object_unref) PopplerPage *page =
            poppler_document_get_page(state->document, state->current_page);
        poppler_page_get_size(page, &page_width, &page_height);
    }
    double new_scale =
        compute_scale(state->ctx, state->num_ctx, page_width, page_height);
    if (fabs(new_scale - state->current_scale) > 0.01) {
        state->current_scale = new_scale;
        fprintf(stderr, "Window resized, new scale: %.2f\n", new_scale);
        free_all_cache_entries(state->cache_entries, state->num_pages);
        state->next_cache_index = 0;
        state->cache_entries[state->current_page] =
            create_cache_entry(state->current_page, state);
        state->needs_redraw = 1;
    }
}

static void update_window_textures(struct prog_state *state) {
    struct render_cache_entry *entry =
        state->cache_entries[state->current_page];
    expect(entry);
    int base_split = entry->img_width / state->num_ctx;
    for (int i = 0; i < state->num_ctx; i++) {
        int offset = i * base_split;
        int region_width = (i == state->num_ctx - 1)
                               ? (entry->img_width - offset)
                               : base_split;
        SDL_Renderer *renderer = state->ctx[i].renderer;
        struct texture_data *texdata = &state->ctx[i].texture;
        SDL_PixelFormatEnum pixel_fmt = SDL_PIXELFORMAT_ARGB8888;
        if (texdata->texture == NULL ||
            texdata->natural_width != region_width ||
            texdata->natural_height != entry->img_height) {
            if (texdata->texture)
                SDL_DestroyTexture(texdata->texture);
            texdata->texture = SDL_CreateTexture(
                renderer, pixel_fmt, SDL_TEXTUREACCESS_STREAMING, region_width,
                entry->img_height);
            expect(texdata->texture);
            texdata->natural_width = region_width;
            texdata->natural_height = entry->img_height;
        }
        int bytes_per_pixel = SDL_BYTESPERPIXEL(pixel_fmt);
        int cairo_stride = cairo_image_surface_get_stride(entry->cairo_surface);
        unsigned char *cairo_data =
            cairo_image_surface_get_data(entry->cairo_surface);
        expect(cairo_data);
        unsigned char *region_data = cairo_data + offset * bytes_per_pixel;
        expect(SDL_UpdateTexture(texdata->texture, NULL, region_data,
                                 cairo_stride) == 0);
        present_texture(renderer, texdata->texture, region_width,
                        entry->img_height);
    }
    state->needs_redraw = 0;
}

static void key_handler(const SDL_Event *event, struct prog_state *state,
                        int *running) {
    const SDL_Keycode key = event->key.keysym.sym;
    const Uint16 mod = event->key.keysym.mod;

    if (key == SDLK_q && (mod & KMOD_SHIFT)) {
        *running = 0;
    } else if (key == SDLK_f && (mod & KMOD_SHIFT)) {
        SDL_Window *win = SDL_GetWindowFromID(event->key.windowID);
        for (int i = 0; i < state->num_ctx; i++) {
            if (win == state->ctx[i].window) {
                toggle_fullscreen(&state->ctx[i]);
                break;
            }
        }
        update_scale(state);
    } else {
        int new_page = state->current_page;
        if ((key == SDLK_LEFT || key == SDLK_UP || key == SDLK_PAGEUP) &&
            state->current_page > 0) {
            new_page = state->current_page - 1;
        } else if ((key == SDLK_RIGHT || key == SDLK_DOWN ||
                    key == SDLK_PAGEDOWN) &&
                   state->current_page < state->num_pages - 1) {
            new_page = state->current_page + 1;
        }
        if (new_page != state->current_page) {
            if (!state->cache_entries[new_page]) {
                fprintf(stderr,
                        "Warning: Page %d not cached; performing blocking "
                        "render.\n",
                        new_page);
                state->cache_entries[new_page] =
                    create_cache_entry(new_page, state);
            }
            state->current_page = new_page;
            state->needs_redraw = 1;
        }
    }
}

static void handle_sdl_events(struct prog_state *state) {
    int running = 1;
    while (running) {
        if (cache_complete(state)) { // Otherwise go ahead to complete cache
            SDL_WaitEvent(NULL);
        }

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
                case SDL_QUIT:
                    running = 0;
                    break;

                case SDL_KEYDOWN:
                    key_handler(&event, state, &running);
                    break;

                case SDL_WINDOWEVENT:
                    if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                        event.window.event == SDL_WINDOWEVENT_RESIZED) {
                        update_scale(state);
                    } else if (event.window.event == SDL_WINDOWEVENT_EXPOSED ||
                               event.window.event == SDL_WINDOWEVENT_SHOWN ||
                               event.window.event == SDL_WINDOWEVENT_RESTORED) {
                        state->needs_redraw = 1;
                    }
                    break;

                default:
                    break;
            }
        }

        if (state->needs_redraw) {
            update_window_textures(state);
        } else {
            cache_one_slide(state->cache_entries, state->num_pages, state);
        }
    }
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <pdf_file>\n", argv[0]);
        return EXIT_FAILURE;
    }
    const char *pdf_file = argv[1];

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "2");
    expect(SDL_Init(SDL_INIT_VIDEO) == 0);

    struct prog_state ps;
    if (init_prog_state(&ps, pdf_file) < 0) {
        SDL_Quit();
        return EXIT_FAILURE;
    }

    ps.num_ctx = NUM_CONTEXTS;
    ps.ctx = calloc(ps.num_ctx, sizeof(struct sdl_ctx));
    expect(ps.ctx);
    create_contexts(ps.ctx, ps.num_ctx);

    ps.current_scale = compute_scale(ps.ctx, ps.num_ctx, ps.init_pdf_width,
                                     ps.init_pdf_height);

    ps.cache_entries =
        calloc(ps.num_pages, sizeof(struct render_cache_entry *));
    expect(ps.cache_entries);

    // Render the first page blocking, we need it immediately.
    ps.cache_entries[ps.current_page] =
        create_cache_entry(ps.current_page, &ps);
    update_window_textures(&ps);

    handle_sdl_events(&ps);

    free_all_cache_entries(ps.cache_entries, ps.num_pages);
    free(ps.cache_entries);
    g_object_unref(ps.document);
    for (int i = 0; i < ps.num_ctx; i++) {
        SDL_DestroyTexture(ps.ctx[i].texture.texture);
        SDL_DestroyRenderer(ps.ctx[i].renderer);
        SDL_DestroyWindow(ps.ctx[i].window);
    }
    free(ps.ctx);
    SDL_Quit();
}
