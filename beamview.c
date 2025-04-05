#include <SDL2/SDL.h>
#include <cairo.h>
#include <errno.h>
#include <glib.h>
#include <limits.h>
#include <math.h>
#include <poppler.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

DEFINE_DROP_FUNC(SDL_Surface *, SDL_FreeSurface)
DEFINE_DROP_FUNC(cairo_surface_t *, cairo_surface_destroy)
DEFINE_DROP_FUNC_COERCE(GObject *, g_object_unref)

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
    int is_fullscreen, windowed_x, windowed_y, windowed_width, windowed_height;
};

struct render_cache_entry {
    SDL_Texture *textures[NUM_CONTEXTS];
    int page_index, widths[NUM_CONTEXTS], texture_height;
};

struct prog_state {
    struct sdl_ctx *ctx;
    double pdf_width, pdf_height, current_scale;
    PopplerDocument *document;
    int num_ctx, current_page, num_pages, next_cache_index, needs_redraw;
    struct render_cache_entry **cache_entries;
};

static void toggle_fullscreen(struct sdl_ctx *ctx) {
    if (ctx->is_fullscreen) {
        SDL_SetWindowFullscreen(ctx->window, 0);
        SDL_SetWindowPosition(ctx->window, ctx->windowed_x, ctx->windowed_y);
        SDL_SetWindowSize(ctx->window, ctx->windowed_width,
                          ctx->windowed_height);
    } else {
        SDL_GetWindowPosition(ctx->window, &ctx->windowed_x, &ctx->windowed_y);
        SDL_GetWindowSize(ctx->window, &ctx->windowed_width,
                          &ctx->windowed_height);
        SDL_SetWindowFullscreen(ctx->window, SDL_WINDOW_FULLSCREEN_DESKTOP);
    }
    ctx->is_fullscreen = !ctx->is_fullscreen;
}

static void present_texture(SDL_Renderer *renderer, SDL_Texture *texture,
                            int natural_width, int natural_height) {
    if (!texture)
        return;

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

static double compute_scale(struct sdl_ctx ctx[], int num_ctx, double pdf_width,
                            double pdf_height) {
    if (pdf_width <= 0 || pdf_height <= 0) {
        fprintf(stderr, "Invalid PDF dimensions: width=%.2f, height=%.2f\n",
                pdf_width, pdf_height);
        return 1.0;
    }
    double scale = 0;
    for (int i = 0; i < num_ctx; i++) {
        int win_width, win_height;
        SDL_GetWindowSize(ctx[i].window, &win_width, &win_height);
        double scale_i = fmax((double)win_width / (pdf_width / num_ctx),
                              (double)win_height / pdf_height);
        scale = fmax(scale, scale_i);
    }
    return scale;
}

static cairo_surface_t *render_page_to_cairo_surface(PopplerPage *page,
                                                     double scale,
                                                     int *img_width,
                                                     int *img_height) {
    double page_width, page_height;
    poppler_page_get_size(page, &page_width, &page_height);
    *img_width = (int)(page_width * scale);
    *img_height = (int)(page_height * scale);

    cairo_surface_t *surface = cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32, *img_width, *img_height);
    cairo_t *cr = cairo_create(surface);
    expect(cairo_status(cr) == CAIRO_STATUS_SUCCESS);

    cairo_set_antialias(cr, CAIRO_ANTIALIAS_BEST);
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_paint(cr);

    cairo_scale(cr, scale, scale);
    poppler_page_render(page, cr);

    cairo_pattern_t *pattern = cairo_get_source(cr);
    cairo_pattern_set_filter(pattern, CAIRO_FILTER_BEST);

    cairo_surface_flush(surface);
    cairo_destroy(cr);

    return surface;
}

static SDL_Surface *
create_sdl_surface_from_cairo(cairo_surface_t *cairo_surface, int x_offset,
                              int region_width, int img_height) {
    int cairo_width = cairo_image_surface_get_width(cairo_surface);
    if (x_offset < 0 || region_width <= 0 ||
        x_offset + region_width > cairo_width) {
        fprintf(stderr,
                "Requested region exceeds cairo surface bounds: x_offset=%d, "
                "region_width=%d, cairo_width=%d\n",
                x_offset, region_width, cairo_width);
        return NULL;
    }

    SDL_Surface *sdl_surface =
        SDL_CreateRGBSurface(0, region_width, img_height, 32, 0x00FF0000,
                             0x0000FF00, 0x000000FF, 0xFF000000);
    if (!sdl_surface)
        return NULL;

    int cairo_stride = cairo_image_surface_get_stride(cairo_surface);
    unsigned char *cairo_data = cairo_image_surface_get_data(cairo_surface);
    expect(cairo_data);

    for (int y = 0; y < img_height; y++) {
        unsigned char *src_row = cairo_data + y * cairo_stride + x_offset * 4;
        unsigned char *dst_row =
            (unsigned char *)sdl_surface->pixels + y * sdl_surface->pitch;
        memcpy(dst_row, src_row, region_width * 4);
    }
    return sdl_surface;
}

static void free_cache_entry(struct render_cache_entry *entry) {
    if (!entry)
        return;
    for (int i = 0; i < NUM_CONTEXTS; i++) {
        if (entry->textures[i])
            SDL_DestroyTexture(entry->textures[i]);
    }
    free(entry);
}

static struct render_cache_entry *create_cache_entry(int page_index,
                                                     struct prog_state *state) {
    _drop_(g_object_unref) PopplerPage *page =
        poppler_document_get_page(state->document, page_index);
    if (!page) {
        fprintf(stderr, "Failed to get page %d\n", page_index);
        return NULL;
    }

    int img_width, img_height;
    _drop_(cairo_surface_destroy) cairo_surface_t *cairo_surface =
        render_page_to_cairo_surface(page, state->current_scale, &img_width,
                                     &img_height);

    struct render_cache_entry *entry = calloc(1, sizeof(*entry));
    expect(entry);
    entry->page_index = page_index;

    int base_split = img_width / state->num_ctx;
    entry->texture_height = img_height;

    for (int i = 0; i < state->num_ctx; i++) {
        int offset = i * base_split;
        int region_width = (i == state->num_ctx - 1)
                               ? (img_width - base_split * i)
                               : base_split;

        _drop_(SDL_FreeSurface) SDL_Surface *surface =
            create_sdl_surface_from_cairo(cairo_surface, offset, region_width,
                                          img_height);
        if (!surface) {
            fprintf(stderr,
                    "Failed to create SDL surface for page %d, region %d\n",
                    page_index, i);
            for (int j = 0; j < i; j++) {
                if (entry->textures[j]) {
                    SDL_DestroyTexture(entry->textures[j]);
                }
            }
            free(entry);
            return NULL;
        }

        entry->widths[i] = region_width;
        entry->textures[i] =
            SDL_CreateTextureFromSurface(state->ctx[i].renderer, surface);
        if (!entry->textures[i]) {
            fprintf(
                stderr,
                "Failed to create SDL texture for page %d, context %d: %s\n",
                page_index, i, SDL_GetError());
            for (int j = 0; j < i; j++) {
                if (entry->textures[j])
                    SDL_DestroyTexture(entry->textures[j]);
            }
            free(entry);
            return NULL;
        }
    }

    fprintf(stderr, "Cache page %d created.\n", page_index);
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
    char *uri = g_strdup_printf("file://%s", resolved_path);
    GError *error = NULL;
    state->document = poppler_document_new_from_file(uri, NULL, &error);
    g_free(uri);

    if (!state->document) {
        fprintf(stderr, "Error opening PDF: %s\n", error->message);
        g_error_free(error);
        return -EIO;
    }

    int num_pages = poppler_document_get_n_pages(state->document);
    if (num_pages <= 0) {
        fprintf(stderr, "PDF has no pages.\n");
        g_object_unref(state->document);
        state->document = NULL;
        return -EINVAL;
    }
    state->num_pages = num_pages;

    _drop_(g_object_unref) PopplerPage *first_page =
        poppler_document_get_page(state->document, 0);
    if (!first_page) {
        fprintf(stderr, "Failed to load first page.\n");
        g_object_unref(state->document);
        state->document = NULL;
        return -ENOENT;
    }
    poppler_page_get_size(first_page, &state->pdf_width, &state->pdf_height);

    state->current_scale = 1.0;

    return 0;
}

static void create_contexts(struct sdl_ctx ctx[], int num_ctx, double pdf_width,
                            double pdf_height) {
    int base = (int)pdf_width / num_ctx;
    for (int i = 0; i < num_ctx; i++) {
        int width = (i == num_ctx - 1) ? (int)pdf_width - base * i : base;
        char title[32];
        snprintf(title, sizeof(title), "Context %d", i);
        ctx[i].window = SDL_CreateWindow(
            title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width,
            (int)pdf_height, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
        expect(ctx[i].window);

        ctx[i].renderer = SDL_CreateRenderer(ctx[i].window, -1,
                                             SDL_RENDERER_ACCELERATED |
                                                 SDL_RENDERER_PRESENTVSYNC);
        expect(ctx[i].renderer);
    }
}

static void update_scale(struct prog_state *state) {
    double new_scale = compute_scale(state->ctx, state->num_ctx,
                                     state->pdf_width, state->pdf_height);
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
    for (int i = 0; i < state->num_ctx; i++) {
        present_texture(state->ctx[i].renderer, entry->textures[i],
                        entry->widths[i], entry->texture_height);
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
    state->cache_entries =
        calloc(state->num_pages, sizeof(struct render_cache_entry *));
    expect(state->cache_entries);

    // Render the first page blocking, we need it immediately.
    state->cache_entries[state->current_page] =
        create_cache_entry(state->current_page, state);
    if (state->cache_entries[state->current_page])
        update_window_textures(state);

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

        cache_one_slide(state->cache_entries, state->num_pages, state);

        if (state->needs_redraw && state->cache_entries[state->current_page])
            update_window_textures(state);
    }
    free_all_cache_entries(state->cache_entries, state->num_pages);
    free(state->cache_entries);
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
    create_contexts(ps.ctx, ps.num_ctx, ps.pdf_width, ps.pdf_height);

    ps.current_scale =
        compute_scale(ps.ctx, ps.num_ctx, ps.pdf_width, ps.pdf_height);

    handle_sdl_events(&ps);

    g_object_unref(ps.document);
    for (int i = 0; i < ps.num_ctx; i++) {
        if (ps.ctx[i].renderer)
            SDL_DestroyRenderer(ps.ctx[i].renderer);
        if (ps.ctx[i].window)
            SDL_DestroyWindow(ps.ctx[i].window);
    }
    free(ps.ctx);
    SDL_Quit();
}
