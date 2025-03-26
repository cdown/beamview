#include <SDL2/SDL.h>
#include <cairo.h>
#include <errno.h>
#include <glib.h>
#include <limits.h>
#include <math.h>
#include <poppler/glib/poppler.h>
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
DEFINE_DROP_FUNC(SDL_Texture *, SDL_DestroyTexture)
DEFINE_DROP_FUNC(SDL_Renderer *, SDL_DestroyRenderer)
DEFINE_DROP_FUNC(SDL_Window *, SDL_DestroyWindow)

#define NUM_CONTEXTS 2

/**
 * Holds SDL texture information and its natural dimensions.
 *
 * @texture: The SDL texture.
 * @natural_width: The original width of the texture.
 * @natural_height: The original height of the texture.
 */
struct texture_data {
    SDL_Texture *texture;
    int natural_width;
    int natural_height;
};

/* Represents an SDL context, including window, renderer, and texture data. */
struct sdl_ctx {
    SDL_Window *window;
    SDL_Renderer *renderer;
    struct texture_data texture;
};

/**
 * Holds the program state including contexts, PDF dimensions, scale, and
 * document.
 *
 * @ctx: Array of SDL contexts.
 * @num_ctx: Number of SDL contexts.
 * @pdf_width: The intrinsic PDF width.
 * @pdf_height: The intrinsic PDF height.
 * @current_scale: The scale factor applied for rendering.
 * @document: The PopplerDocument representing the PDF.
 */
struct prog_state {
    struct sdl_ctx *ctx;
    int num_ctx;
    double pdf_width;
    double pdf_height;
    double current_scale;
    PopplerDocument *document;
};

/**
 * Present a texture on a renderer while maintaining its aspect ratio.
 *
 * @renderer: The SDL renderer.
 * @texture: The SDL texture to present.
 * @natural_width: The original width of the texture.
 * @natural_height: The original height of the texture.
 */
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

/**
 * Compute the required scale factor based on the intrinsic PDF dimensions and
 * the sizes of SDL windows in multiple contexts.
 *
 * @ctx: Array of SDL contexts.
 * @num_ctx: Number of contexts.
 * @pdf_width: The intrinsic PDF width.
 * @pdf_height: The intrinsic PDF height.
 */
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
        if (scale_i > scale)
            scale = scale_i;
    }
    return scale;
}

/**
 * Render a PDF page to a Cairo image surface.
 *
 * @page: The PopplerPage to render.
 * @scale: The scale factor to apply.
 * @img_width: Output pointer for the resulting image width.
 * @img_height: Output pointer for the resulting image height.
 */
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
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_BEST);

    // If the background is transparent, the user probably expects white
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);

    cairo_scale(cr, scale, scale);
    poppler_page_render(page, cr);
    cairo_surface_flush(surface);
    cairo_destroy(cr);

    return surface;
}

/**
 * Create an SDL surface from a region of a Cairo image surface.
 *
 * @cairo_surface: The source Cairo surface.
 * @x_offset: The starting x offset in pixels.
 * @region_width: The width of the region to extract.
 * @img_height: The height of the image.
 */
static SDL_Surface *
create_sdl_surface_from_cairo(cairo_surface_t *cairo_surface, int x_offset,
                              int region_width, int img_height) {
    int cairo_width = cairo_image_surface_get_width(cairo_surface);
    if (x_offset < 0 || region_width <= 0 ||
        x_offset + region_width > cairo_width) {
        fprintf(
            stderr,
            "Requested region exceeds cairo surface bounds: x_offset %d, region_width %d, cairo_width %d\n",
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

    for (int y = 0; y < img_height; y++) {
        unsigned char *src_row = cairo_data + y * cairo_stride + x_offset * 4;
        memcpy((unsigned char *)sdl_surface->pixels + y * sdl_surface->pitch,
               src_row, region_width * 4);
    }

    return sdl_surface;
}

/**
 * Represents a cached rendered page.
 *
 * @page_index: The page index corresponding to this entry.
 * @textures: Array of SDL textures for each context.
 * @widths: Array of natural widths for each texture.
 * @texture_height: Natural height of the combined page.
 */
struct render_cache_entry {
    int page_index;
    SDL_Texture *textures[NUM_CONTEXTS];
    int widths[NUM_CONTEXTS];
    int texture_height;
};

/**
 * Holds cached pages for previous, current, and next slides.
 *
 * If a slide does not exist (e.g., at the beginning or end), the pointer is
 * NULL.
 *
 * @prev: Cache entry for the previous page.
 * @cur: Cache entry for the current page.
 * @next: Cache entry for the next page.
 */
struct render_cache {
    struct render_cache_entry *prev;
    struct render_cache_entry *cur;
    struct render_cache_entry *next;
};

/**
 * Render and create a cache entry for a given page index.
 *
 * Added debug output to indicate when a page is rendered live.
 *
 * @page_index: The index of the page to render.
 * @state: Pointer to the program state.
 * @ctx: Array of SDL contexts.
 *
 * @return A new render_cache_entry for the requested page, or NULL on failure.
 */
static struct render_cache_entry *create_cache_entry(int page_index,
                                                     struct prog_state *state,
                                                     struct sdl_ctx ctx[],
                                                     int num_ctx) {
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

    struct render_cache_entry *entry =
        malloc(sizeof(struct render_cache_entry));
    if (!entry) {
        return NULL;
    }
    entry->page_index = page_index;
    entry->texture_height = img_height;
    int base_width = img_width / num_ctx;
    for (int i = 0; i < num_ctx; i++) {
        int x_offset = i * base_width;
        int region_width =
            (i == num_ctx - 1) ? (img_width - base_width * i) : base_width;
        SDL_Surface *surface = create_sdl_surface_from_cairo(
            cairo_surface, x_offset, region_width, img_height);
        if (!surface) {
            fprintf(stderr,
                    "Failed to create SDL surface for page %d, context %d\n",
                    page_index, i);
            for (int j = 0; j < i; j++) {
                if (entry->textures[j])
                    SDL_DestroyTexture(entry->textures[j]);
            }
            free(entry);
            return NULL;
        }
        entry->widths[i] = region_width;
        entry->textures[i] =
            SDL_CreateTextureFromSurface(ctx[i].renderer, surface);
        if (!entry->textures[i]) {
            fprintf(
                stderr,
                "Failed to create SDL texture for page %d, context %d: %s\n",
                page_index, i, SDL_GetError());
            SDL_FreeSurface(surface);
            for (int j = 0; j < i; j++) {
                if (entry->textures[j])
                    SDL_DestroyTexture(entry->textures[j]);
            }
            free(entry);
            return NULL;
        }
        SDL_FreeSurface(surface);
    }
    return entry;
}

/**
 * Free a render cache entry and its associated textures.
 *
 * @entry: The render cache entry to free.
 */
static void free_cache_entry(struct render_cache_entry *entry) {
    if (!entry)
        return;
    for (int i = 0; i < NUM_CONTEXTS; i++) {
        if (entry->textures[i])
            SDL_DestroyTexture(entry->textures[i]);
    }
    free(entry);
}

/**
 * Initialise the cache for the current page.
 *
 * This renders the current page synchronously and clears any neighbour entries.
 *
 * @cache: The render cache to initialise.
 * @current_page: The current page index.
 * @state: Pointer to the program state.
 */
static void init_cache_for_page(struct render_cache *cache, int current_page,
                                struct prog_state *state) {
    if (cache->prev) {
        free_cache_entry(cache->prev);
        cache->prev = NULL;
    }
    if (cache->cur) {
        free_cache_entry(cache->cur);
        cache->cur = NULL;
    }
    if (cache->next) {
        free_cache_entry(cache->next);
        cache->next = NULL;
    }
    cache->cur =
        create_cache_entry(current_page, state, state->ctx, state->num_ctx);
}

/**
 * When idle (i.e., no events for 50ms), update the cache by creating neighbour
 * entries if needed.
 *
 * @cache: The render cache structure.
 * @current_page: The current page index.
 * @state: Pointer to the program state.
 * @num_pages: The total number of pages in the document.
 */
static void update_cache(struct render_cache *cache, int current_page,
                         struct prog_state *state, int num_pages) {
    if (cache->cur == NULL)
        return;
    if (current_page > 0 && cache->prev == NULL) {
        cache->prev = create_cache_entry(current_page - 1, state, state->ctx,
                                         state->num_ctx);
    }
    if (current_page < num_pages - 1 && cache->next == NULL) {
        cache->next = create_cache_entry(current_page + 1, state, state->ctx,
                                         state->num_ctx);
    }
}

/**
 * Update the cache when a page change occurs.
 *
 * If the new page is already cached (in prev or next), shift the cache window.
 * Otherwise, reinitialise the cache for the new page, rendering only the
 * current page. Note: neighbour pages are not pre-rendered during keypress;
 * update_cache will handle that during idle.
 */
static void shift_cache_entries(struct render_cache *cache, int new_page,
                                struct prog_state *state) {
    if (cache->cur && cache->cur->page_index == new_page) {
        // Cache is already current?
        return;
    }
    if (cache->next && cache->next->page_index == new_page) {
        if (cache->prev) {
            free_cache_entry(cache->prev);
        }
        cache->prev = cache->cur;
        cache->cur = cache->next;
        cache->next = NULL;
        return;
    }
    if (cache->prev && cache->prev->page_index == new_page) {
        if (cache->next) {
            free_cache_entry(cache->next);
        }
        cache->next = cache->cur;
        cache->cur = cache->prev;
        cache->prev = NULL;
        return;
    }
    fprintf(stderr, "Cache not ready for page %d\n", new_page);
    init_cache_for_page(cache, new_page, state);
}

/**
 * Apply the textures from the current cache entry to the program state.
 *
 * @state: Pointer to the program state.
 * @entry: The current cache entry.
 */
static void apply_cache_entry_to_state(struct prog_state *state,
                                       struct render_cache_entry *entry) {
    for (int i = 0; i < state->num_ctx; i++) {
        state->ctx[i].texture.texture = entry->textures[i];
        state->ctx[i].texture.natural_width = entry->widths[i];
        state->ctx[i].texture.natural_height = entry->texture_height;
    }
}

/**
 * Initialise the program state by loading the PDF, resolving its intrinsic
 * dimensions, and setting default values.
 *
 * @state: Pointer to the program state.
 * @pdf_file: Path to the PDF file.
 */
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

/**
 * Create an SDL window with the given title and dimensions.
 *
 * @title: The window title.
 * @width: The window width.
 * @height: The window height.
 */
static SDL_Window *create_window(const char *title, int width, int height) {
    return SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED,
                            SDL_WINDOWPOS_CENTERED, width, height,
                            SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
}

/**
 * Create SDL contexts (windows) based on the intrinsic PDF dimensions.
 *
 * @ctx: Array of SDL contexts to populate.
 * @num_ctx: Number of contexts.
 * @pdf_width: The intrinsic PDF width.
 * @pdf_height: The intrinsic PDF height.
 */
static int create_contexts(struct sdl_ctx ctx[], int num_ctx, double pdf_width,
                           double pdf_height) {
    int init_img_width = (int)pdf_width;
    int init_img_height = (int)pdf_height;
    int widths[num_ctx];
    for (int i = 0; i < num_ctx; i++) {
        widths[i] = init_img_width / num_ctx;
    }
    widths[num_ctx - 1] =
        init_img_width - (init_img_width / num_ctx) * (num_ctx - 1);

    for (int i = 0; i < num_ctx; i++) {
        char title[32];
        snprintf(title, sizeof(title), "Context %d", i);
        ctx[i].window = create_window(title, widths[i], init_img_height);
        if (!ctx[i].window) {
            fprintf(stderr, "Failed to create SDL window for context %d: %s\n",
                    i, SDL_GetError());
            for (int j = 0; j < i; j++) {
                SDL_DestroyWindow(ctx[j].window);
            }
            return -EIO;
        }
    }
    return 0;
}

/**
 * Create SDL renderers for the provided contexts.
 *
 * @ctx: Array of SDL contexts.
 * @num_ctx: Number of contexts.
 */
static int create_context_renderers(struct sdl_ctx ctx[], int num_ctx) {
    for (int i = 0; i < num_ctx; i++) {
        ctx[i].renderer = SDL_CreateRenderer(ctx[i].window, -1,
                                             SDL_RENDERER_ACCELERATED |
                                                 SDL_RENDERER_PRESENTVSYNC);
        if (!ctx[i].renderer) {
            fprintf(stderr,
                    "Failed to create SDL renderer for context %d: %s\n", i,
                    SDL_GetError());
            for (int j = 0; j < i; j++) {
                if (ctx[j].renderer)
                    SDL_DestroyRenderer(ctx[j].renderer);
            }
            return -EIO;
        }
    }
    return 0;
}

/**
 * Handle SDL events in a loop, including key and window events, and render page
 * changes.
 *
 * @state: Pointer to the program state.
 * @num_pages: The total number of pages in the PDF.
 */
static int handle_sdl_events(struct prog_state *state, int num_pages) {
    int running = 1;
    int pending_quit = 0;
    int current_page = 0;
    Uint32 window_ids[NUM_CONTEXTS];
    for (int i = 0; i < state->num_ctx; i++) {
        window_ids[i] = SDL_GetWindowID(state->ctx[i].window);
    }
    Uint32 last_activity = SDL_GetTicks();

    struct render_cache cache = {0};
    init_cache_for_page(&cache, 0, state);
    if (cache.cur) {
        apply_cache_entry_to_state(state, cache.cur);
        for (int i = 0; i < state->num_ctx; i++) {
            present_texture(state->ctx[i].renderer,
                            state->ctx[i].texture.texture,
                            state->ctx[i].texture.natural_width,
                            state->ctx[i].texture.natural_height);
        }
    }

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
                case SDL_QUIT:
                    running = 0;
                    break;
                case SDL_KEYDOWN:
                    if (event.key.keysym.sym == SDLK_q) {
                        if (pending_quit)
                            running = 0;
                        else
                            pending_quit = 1;
                    } else {
                        pending_quit = 0;
                        int new_page = current_page;
                        if (event.key.keysym.sym == SDLK_LEFT ||
                            event.key.keysym.sym == SDLK_UP ||
                            event.key.keysym.sym == SDLK_PAGEUP) {
                            if (current_page > 0)
                                new_page = current_page - 1;
                        } else if (event.key.keysym.sym == SDLK_RIGHT ||
                                   event.key.keysym.sym == SDLK_DOWN ||
                                   event.key.keysym.sym == SDLK_PAGEDOWN) {
                            if (current_page < num_pages - 1)
                                new_page = current_page + 1;
                        }
                        if (new_page != current_page) {
                            current_page = new_page;
                            last_activity = SDL_GetTicks();
                            // Update only the current page; neighbour updates
                            // are deferred until idle
                            shift_cache_entries(&cache, current_page, state);
                            if (cache.cur) {
                                apply_cache_entry_to_state(state, cache.cur);
                                for (int i = 0; i < state->num_ctx; i++) {
                                    present_texture(
                                        state->ctx[i].renderer,
                                        state->ctx[i].texture.texture,
                                        state->ctx[i].texture.natural_width,
                                        state->ctx[i].texture.natural_height);
                                }
                            } else {
                                fprintf(stderr, "Error rendering page %d\n",
                                        current_page);
                            }
                        }
                    }
                    break;
                case SDL_WINDOWEVENT:
                    if (event.window.event == SDL_WINDOWEVENT_RESIZED) {
                        double new_scale =
                            compute_scale(state->ctx, state->num_ctx,
                                          state->pdf_width, state->pdf_height);
                        if (fabs(new_scale - state->current_scale) > 0.01) {
                            state->current_scale = new_scale;
                            fprintf(stderr, "Window resized, new scale: %.2f\n",
                                    new_scale);
                            init_cache_for_page(&cache, current_page, state);
                            if (cache.cur) {
                                apply_cache_entry_to_state(state, cache.cur);
                                for (int i = 0; i < state->num_ctx; i++) {
                                    present_texture(
                                        state->ctx[i].renderer,
                                        state->ctx[i].texture.texture,
                                        state->ctx[i].texture.natural_width,
                                        state->ctx[i].texture.natural_height);
                                }
                            }
                        }
                    } else if (event.window.event == SDL_WINDOWEVENT_EXPOSED ||
                               event.window.event == SDL_WINDOWEVENT_SHOWN ||
                               event.window.event == SDL_WINDOWEVENT_RESTORED) {
                        for (int i = 0; i < state->num_ctx; i++) {
                            if (event.window.windowID == window_ids[i])
                                present_texture(
                                    state->ctx[i].renderer,
                                    state->ctx[i].texture.texture,
                                    state->ctx[i].texture.natural_width,
                                    state->ctx[i].texture.natural_height);
                        }
                    }
                    break;
                default:
                    break;
            }
        }

        if (SDL_GetTicks() - last_activity > 50) {
            update_cache(&cache, current_page, state, num_pages);
        }

        SDL_Delay(10);
    }

    if (cache.prev)
        free_cache_entry(cache.prev);
    if (cache.cur)
        free_cache_entry(cache.cur);
    if (cache.next)
        free_cache_entry(cache.next);

    return 0;
}

int main(int argc, char *argv[]) {
    int ret = 0;

    if (argc < 2) {
        fprintf(stderr, "Usage: %s <pdf_file>\n", argv[0]);
        return EXIT_FAILURE;
    }

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "2");

    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL could not initialise: %s\n", SDL_GetError());
        return EXIT_FAILURE;
    }

    struct prog_state ps;
    ret = init_prog_state(&ps, argv[1]);
    if (ret < 0) {
        SDL_Quit();
        return EXIT_FAILURE;
    }

    ps.num_ctx = NUM_CONTEXTS;
    ps.ctx = calloc(ps.num_ctx, sizeof(struct sdl_ctx));
    if (!ps.ctx) {
        fprintf(stderr, "Failed to allocate SDL contexts.\n");
        if (ps.document) {
            g_object_unref(ps.document);
            ps.document = NULL;
        }
        SDL_Quit();
        return EXIT_FAILURE;
    }

    ret = create_contexts(ps.ctx, ps.num_ctx, ps.pdf_width, ps.pdf_height);
    if (ret < 0) {
        free(ps.ctx);
        SDL_Quit();
        return EXIT_FAILURE;
    }

    ret = create_context_renderers(ps.ctx, ps.num_ctx);
    if (ret < 0) {
        for (int i = 0; i < ps.num_ctx; i++) {
            if (ps.ctx[i].window)
                SDL_DestroyWindow(ps.ctx[i].window);
        }
        free(ps.ctx);
        SDL_Quit();
        return EXIT_FAILURE;
    }

    ps.current_scale =
        compute_scale(ps.ctx, ps.num_ctx, ps.pdf_width, ps.pdf_height);

    int num_pages = poppler_document_get_n_pages(ps.document);
    ret = handle_sdl_events(&ps, num_pages);
    if (ret < 0) {
        SDL_Quit();
        return EXIT_FAILURE;
    }

    if (ps.document) {
        g_object_unref(ps.document);
        ps.document = NULL;
    }

    for (int i = 0; i < ps.num_ctx; i++) {
        if (ps.ctx[i].renderer)
            SDL_DestroyRenderer(ps.ctx[i].renderer);
        if (ps.ctx[i].window)
            SDL_DestroyWindow(ps.ctx[i].window);
    }
    free(ps.ctx);
    SDL_Quit();
    return EXIT_SUCCESS;
}
