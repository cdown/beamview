#include <SDL2/SDL.h>
#include <X11/Xlib.h>
#include <cairo.h>
#include <glib.h>
#include <limits.h>
#include <math.h>
#include <poppler.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define die_on(cond, fmt, ...)                                                 \
    do {                                                                       \
        if (cond) {                                                            \
            fprintf(stderr, "FATAL: " fmt, ##__VA_ARGS__);                     \
            exit(1);                                                           \
        }                                                                      \
    } while (0)
#define expect(x)                                                              \
    die_on(!(x), "!(%s) at %s:%s:%d\n", #x, __FILE__, __func__, __LINE__)

static const int page_number_invalid = -1;
#define CACHE_SIZE 3
#define NUM_CTX 2
#define BV_CTX "bv_ctx"

struct bv_texture {
    SDL_Texture *texture;
    int natural_width, natural_height;
};

struct bv_sdl_ctx {
    SDL_Window *window;
    SDL_Renderer *renderer;
    struct bv_texture texture;
    int is_fullscreen;
    int region_index;
};

struct bv_cache_entry {
    cairo_surface_t *cairo_surface;
    int img_width, img_height;
    double page_width, page_height;
    int page_number;
};

static struct bv_cache_entry *cache_slot(struct bv_cache_entry cache[],
                                         int page) {
    return &cache[page % CACHE_SIZE];
}

struct bv_prog_state {
    struct bv_sdl_ctx ctx[NUM_CTX];
    double current_scale;
    PopplerDocument *document;
    int current_page, num_pages, needs_redraw, needs_cache;
    struct bv_cache_entry page_cache[CACHE_SIZE];
};

static void toggle_fullscreen(struct bv_sdl_ctx *ctx) {
    SDL_SetWindowFullscreen(
        ctx->window, ctx->is_fullscreen ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
    ctx->is_fullscreen = !ctx->is_fullscreen;
}

static void present_texture(SDL_Renderer *renderer, SDL_Texture *texture,
                            int natural_width, int natural_height) {
    expect(texture);

    int win_width, win_height;
    SDL_GetRendererOutputSize(renderer, &win_width, &win_height);

    double scale = fmin((double)win_width / natural_width,
                        (double)win_height / natural_height);
    int new_width = (int)(natural_width * scale);
    int new_height = (int)(natural_height * scale);

    SDL_Rect dst = {(win_width - new_width) / 2, (win_height - new_height) / 2,
                    new_width, new_height};

    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, NULL, &dst);
    SDL_RenderPresent(renderer);
}

static double compute_scale(struct bv_sdl_ctx ctx[], int num_ctx,
                            double page_width, double page_height) {
    expect(page_width > 0 && page_height > 0);
    double scale = 0;
    for (int i = 0; i < num_ctx; i++) {
        int win_width, win_height;
        SDL_GetRendererOutputSize(ctx[i].renderer, &win_width, &win_height);
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
    cairo_t *cr = cairo_create(surface);
    expect(cairo_status(cr) == CAIRO_STATUS_SUCCESS);

    cairo_set_antialias(cr, CAIRO_ANTIALIAS_BEST);
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_paint(cr);

    cairo_scale(cr, scale, scale);
    poppler_page_render(page, cr);
    cairo_surface_flush(surface);
    cairo_destroy(cr);

    return surface;
}

static void invalidate_cache_slot(struct bv_cache_entry *slot) {
    if (slot->cairo_surface)
        cairo_surface_destroy(slot->cairo_surface);
    *slot = (struct bv_cache_entry){.page_number = page_number_invalid};
}

enum cache_result { CACHE_UPDATED, CACHE_REUSED };

static enum cache_result page_cache_update(struct bv_prog_state *state,
                                           int page_index) {
    struct bv_cache_entry *slot = cache_slot(state->page_cache, page_index);

    if (slot->page_number == page_index)
        return CACHE_REUSED;

    PopplerPage *page = poppler_document_get_page(state->document, page_index);
    expect(page);
    invalidate_cache_slot(slot);

    slot->cairo_surface = render_page_to_cairo_surface(
        page, state->current_scale, &slot->img_width, &slot->img_height,
        &slot->page_width, &slot->page_height);

    slot->page_number = page_index;
    g_object_unref(page);

    return CACHE_UPDATED;
}

static void idle_update_cache(struct bv_prog_state *state) {
    if (state->current_page > 0)
        page_cache_update(state, state->current_page - 1);
    if (state->current_page < state->num_pages - 1)
        page_cache_update(state, state->current_page + 1);
    state->needs_cache = 0;
}

static int accel_x11_error_handler(Display *dpy, XErrorEvent *event) {
    (void)dpy;
    (void)event;
    return 0;
}

static SDL_Renderer *create_renderer_with_fallback(SDL_Window *window) {
    int (*old_handler)(Display *, XErrorEvent *) =
        XSetErrorHandler(accel_x11_error_handler); // Avoid BadValue crash
    SDL_Renderer *renderer = SDL_CreateRenderer(
        window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    XSetErrorHandler(old_handler);

    if (!renderer) {
        fprintf(stderr, "Warning: hardware acceleration unavailable\n");
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    }

    return renderer;
}

static void create_contexts(struct bv_sdl_ctx ctx[], int num_ctx) {
    SDL_Rect display_bounds;
    expect(SDL_GetDisplayBounds(0, &display_bounds) == 0);
    const int win_width = 1280;
    const int win_height = 720;
    int center_x = display_bounds.x + (display_bounds.w - win_width) / 2;
    int center_y = display_bounds.y + (display_bounds.h - win_height) / 2;
    const int offset = 100;

    for (int i = 0; i < num_ctx; i++) {
        ctx[i].window = SDL_CreateWindow(
            "beamview", center_x + i * offset, center_y + i * offset, win_width,
            win_height,
            SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
        expect(ctx[i].window);
        ctx[i].region_index = i;
        SDL_SetWindowData(ctx[i].window, BV_CTX, &ctx[i]);
        ctx[i].renderer = create_renderer_with_fallback(ctx[i].window);
        expect(ctx[i].renderer);
    }
}

static void open_document(struct bv_prog_state *state, const char *pdf_file) {
    char resolved_path[PATH_MAX];
    die_on(!realpath(pdf_file, resolved_path), "Couldn't resolve %s\n",
           pdf_file);

    char *uri = g_strdup_printf("file://%s", resolved_path);
    GError *error = NULL;
    state->document = poppler_document_new_from_file(uri, NULL, &error);
    g_free(uri);
    die_on(!state->document, "Error opening PDF: %s\n", error->message);

    state->num_pages = poppler_document_get_n_pages(state->document);
    die_on(state->num_pages <= 0, "PDF has no pages\n");
}

static void init_cache(struct bv_prog_state *state) {
    state->needs_redraw = 1;
    state->needs_cache = 1;
    for (int i = 0; i < CACHE_SIZE; i++)
        invalidate_cache_slot(&state->page_cache[i]);
    page_cache_update(state, state->current_page);
}

static void ensure_texture(struct bv_texture *texdata, SDL_Renderer *renderer,
                           SDL_PixelFormatEnum pixel_fmt, int width,
                           int height) {
    if (texdata->texture == NULL || texdata->natural_width != width ||
        texdata->natural_height != height) {
        if (texdata->texture)
            SDL_DestroyTexture(texdata->texture);
        texdata->texture = SDL_CreateTexture(
            renderer, pixel_fmt, SDL_TEXTUREACCESS_STREAMING, width, height);
        expect(texdata->texture);
        texdata->natural_width = width;
        texdata->natural_height = height;
    }
}

static void update_texture_for_context(struct bv_sdl_ctx *ctx,
                                       struct bv_cache_entry *entry) {
    int base_split = entry->img_width / NUM_CTX;
    int offset = ctx->region_index * base_split;
    int region_width = (ctx->region_index == NUM_CTX - 1)
                           ? (entry->img_width - offset)
                           : base_split;

    SDL_Renderer *renderer = ctx->renderer;
    struct bv_texture *texdata = &ctx->texture;
    SDL_PixelFormatEnum pixel_fmt = SDL_PIXELFORMAT_ARGB8888;
    ensure_texture(texdata, renderer, pixel_fmt, region_width,
                   entry->img_height);

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

static void update_scale(struct bv_prog_state *state) {
    page_cache_update(state, state->current_page);
    struct bv_cache_entry *entry =
        cache_slot(state->page_cache, state->current_page);
    state->current_scale = compute_scale(state->ctx, NUM_CTX, entry->page_width,
                                         entry->page_height);
    init_cache(state);
}

static void handle_fullscreen_event(const SDL_Event *event,
                                    struct bv_prog_state *state) {
    SDL_Window *win = SDL_GetWindowFromID(event->key.windowID);
    struct bv_sdl_ctx *ctx = SDL_GetWindowData(win, BV_CTX);
    toggle_fullscreen(ctx);
    update_scale(state);
}

static void handle_navigation_event(const SDL_Keycode key,
                                    struct bv_prog_state *state) {
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
        if (page_cache_update(state, new_page) == CACHE_UPDATED)
            fprintf(stderr, "Warning: Page %d rendered live\n", new_page);
        state->current_page = new_page;
        state->needs_redraw = 1;
        state->needs_cache = 1;
    }
}

static void init_prog_state(struct bv_prog_state *state, const char *pdf_file) {
    *state = (struct bv_prog_state){0};
    open_document(state, pdf_file);
    init_cache(state);
    create_contexts(state->ctx, NUM_CTX);
    update_scale(state);
}

static void update_window_textures(struct bv_prog_state *state) {
    struct bv_cache_entry *entry =
        cache_slot(state->page_cache, state->current_page);
    expect(entry->cairo_surface);

    for (int i = 0; i < NUM_CTX; i++) {
        update_texture_for_context(&state->ctx[i], entry);
    }

    state->needs_redraw = 0;
}

static void key_handler(const SDL_Event *event, struct bv_prog_state *state,
                        int *running) {
    const SDL_Keycode key = event->key.keysym.sym;
    const Uint16 mod = event->key.keysym.mod;

    if (key == SDLK_q && (mod & KMOD_SHIFT)) {
        *running = 0;
    } else if (key == SDLK_f && (mod & KMOD_SHIFT)) {
        handle_fullscreen_event(event, state);
    } else {
        handle_navigation_event(key, state);
    }
}

static void handle_sdl_events(struct bv_prog_state *state) {
    int running = 1;
    while (running) {
        if (!state->needs_redraw && !state->needs_cache) {
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
                    if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                        update_scale(state);
                    } else if (event.window.event == SDL_WINDOWEVENT_EXPOSED ||
                               event.window.event == SDL_WINDOWEVENT_SHOWN ||
                               event.window.event == SDL_WINDOWEVENT_RESTORED) {
                        state->needs_redraw = 1;
                    }
                    break;
            }
        }

        if (state->needs_redraw) {
            update_window_textures(state);
        }

        idle_update_cache(state);
    }
}

static void free_prog_state(struct bv_prog_state *state) {
    for (int i = 0; i < CACHE_SIZE; i++) {
        invalidate_cache_slot(&state->page_cache[i]);
    }
    for (int i = 0; i < NUM_CTX; i++) {
        SDL_DestroyTexture(state->ctx[i].texture.texture);
        SDL_DestroyRenderer(state->ctx[i].renderer);
        SDL_DestroyWindow(state->ctx[i].window);
    }
    g_object_unref(state->document);
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <pdf_file>\nSee `man 1 beamview`.\n",
                argv[0]);
        return EXIT_FAILURE;
    }

    if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        execlp("man", "man", "1", "beamview", NULL);
        perror("execlp man");
        return EXIT_FAILURE;
    }

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "2");
    expect(SDL_Init(SDL_INIT_VIDEO) == 0);

    struct bv_prog_state ps;
    init_prog_state(&ps, argv[1]);
    handle_sdl_events(&ps);
    free_prog_state(&ps);
}
