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

#define DEFINE_DROP_FUNC_PTR(type, func)                                       \
    static inline void drop_##func(type *p) { func(p); }

#define DEFINE_DROP_FUNC(type, func)                                           \
    static inline void drop_##func(type *p) {                                  \
        if (*p)                                                                \
            func(*p);                                                          \
    }

#define DEFINE_DROP_FUNC_VOID(func)                                            \
    static inline void drop_##func(void *p) {                                  \
        void **pp = p;                                                         \
        if (*pp)                                                               \
            func(*pp);                                                         \
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

/**
 * Holds the program state including textures, PDF dimensions, scale, and
 * document.
 *
 * @left: Texture data for the left half.
 * @right: Texture data for the right half.
 * @pdf_width: The intrinsic width of the PDF.
 * @pdf_height: The intrinsic height of the PDF.
 * @current_scale: The scale factor applied for rendering.
 * @document: The PopplerDocument representing the PDF.
 */
struct prog_state {
    struct texture_data left;
    struct texture_data right;
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
 * the sizes of two SDL windows.
 *
 * @window_left: The SDL window for the left half.
 * @window_right: The SDL window for the right half.
 * @pdf_width: The intrinsic PDF width.
 * @pdf_height: The intrinsic PDF height.
 */
static double compute_scale(SDL_Window *window_left, SDL_Window *window_right,
                            double pdf_width, double pdf_height) {
    if (pdf_width <= 0 || pdf_height <= 0) {
        fprintf(stderr, "Invalid PDF dimensions: width=%.2f, height=%.2f\n",
                pdf_width, pdf_height);
        return 1.0;
    }
    int lw, lh, rw, rh;
    SDL_GetWindowSize(window_left, &lw, &lh);
    SDL_GetWindowSize(window_right, &rw, &rh);

    double scale_left =
        fmax((double)lw / (pdf_width / 2.0), (double)lh / pdf_height);
    double scale_right =
        fmax((double)rw / (pdf_width / 2.0), (double)rh / pdf_height);

    return fmax(scale_left, scale_right);
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
 * @left_texture: The SDL texture for the left half of the page.
 * @right_texture: The SDL texture for the right half of the page.
 * @left_width: Natural width of the left texture.
 * @right_width: Natural width of the right texture.
 * @texture_height: Natural height of the combined page.
 */
struct render_cache_entry {
    int page_index;
    SDL_Texture *left_texture;
    SDL_Texture *right_texture;
    int left_width;
    int right_width;
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
 * @renderer_left: The SDL renderer for the left half.
 * @renderer_right: The SDL renderer for the right half.
 *
 * @return A new render_cache_entry for the requested page, or NULL on failure.
 */
static struct render_cache_entry *
create_cache_entry(int page_index, struct prog_state *state,
                   SDL_Renderer *renderer_left, SDL_Renderer *renderer_right) {
    _drop_(g_object_unref) PopplerPage *page =
        poppler_document_get_page(state->document, page_index);
    if (!page) {
        fprintf(stderr, "Failed to get page %d\n", page_index);
        return NULL;
    }
    double page_width, page_height;
    poppler_page_get_size(page, &page_width, &page_height);

    int img_width, img_height;
    _drop_(cairo_surface_destroy) cairo_surface_t *cairo_surface =
        render_page_to_cairo_surface(page, state->current_scale, &img_width,
                                     &img_height);

    int left_width = img_width / 2;
    int right_width = img_width - left_width;

    _drop_(SDL_FreeSurface) SDL_Surface *left_surface =
        create_sdl_surface_from_cairo(cairo_surface, 0, left_width, img_height);
    _drop_(SDL_FreeSurface) SDL_Surface *right_surface =
        create_sdl_surface_from_cairo(cairo_surface, left_width, right_width,
                                      img_height);
    if (!left_surface || !right_surface) {
        fprintf(stderr, "Failed to create SDL surfaces for page %d\n",
                page_index);
        return NULL;
    }

    SDL_Texture *left_texture =
        SDL_CreateTextureFromSurface(renderer_left, left_surface);
    SDL_Texture *right_texture =
        SDL_CreateTextureFromSurface(renderer_right, right_surface);
    if (!left_texture || !right_texture) {
        fprintf(stderr, "Failed to create SDL textures for page %d: %s\n",
                page_index, SDL_GetError());
        if (left_texture)
            SDL_DestroyTexture(left_texture);
        if (right_texture)
            SDL_DestroyTexture(right_texture);
        return NULL;
    }

    struct render_cache_entry *entry =
        malloc(sizeof(struct render_cache_entry));
    if (!entry) {
        SDL_DestroyTexture(left_texture);
        SDL_DestroyTexture(right_texture);
        return NULL;
    }
    entry->page_index = page_index;
    entry->left_texture = left_texture;
    entry->right_texture = right_texture;
    entry->left_width = left_width;
    entry->right_width = right_width;
    entry->texture_height = img_height;
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
    if (entry->left_texture)
        SDL_DestroyTexture(entry->left_texture);
    if (entry->right_texture)
        SDL_DestroyTexture(entry->right_texture);
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
 * @renderer_left: The SDL renderer for the left half.
 * @renderer_right: The SDL renderer for the right half.
 */
static void init_cache_for_page(struct render_cache *cache, int current_page,
                                struct prog_state *state,
                                SDL_Renderer *renderer_left,
                                SDL_Renderer *renderer_right) {
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
        create_cache_entry(current_page, state, renderer_left, renderer_right);
}

/**
 * When idle (i.e., no events for 50ms), update the cache by creating neighbour
 * entries if needed.
 *
 * @cache: The render cache structure.
 * @current_page: The current page index.
 * @state: Pointer to the program state.
 * @renderer_left: The SDL renderer for the left half.
 * @renderer_right: The SDL renderer for the right half.
 * @num_pages: The total number of pages in the document.
 */
static void update_cache(struct render_cache *cache, int current_page,
                         struct prog_state *state, SDL_Renderer *renderer_left,
                         SDL_Renderer *renderer_right, int num_pages) {
    if (cache->cur == NULL)
        return;
    if (current_page > 0 && cache->prev == NULL) {
        cache->prev = create_cache_entry(current_page - 1, state, renderer_left,
                                         renderer_right);
    }
    if (current_page < num_pages - 1 && cache->next == NULL) {
        cache->next = create_cache_entry(current_page + 1, state, renderer_left,
                                         renderer_right);
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
static void update_cache_on_page_change(struct render_cache *cache,
                                        int new_page, struct prog_state *state,
                                        SDL_Renderer *renderer_left,
                                        SDL_Renderer *renderer_right) {
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
    init_cache_for_page(cache, new_page, state, renderer_left, renderer_right);
}

/**
 * Apply the textures from the current cache entry to the program state.
 *
 * @state: Pointer to the program state.
 * @entry: The current cache entry.
 */
static void apply_cache_entry_to_state(struct prog_state *state,
                                       struct render_cache_entry *entry) {
    state->left.texture = entry->left_texture;
    state->left.natural_width = entry->left_width;
    state->left.natural_height = entry->texture_height;
    state->right.texture = entry->right_texture;
    state->right.natural_width = entry->right_width;
    state->right.natural_height = entry->texture_height;
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

    state->left.texture = NULL;
    state->right.texture = NULL;
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
 * Create two SDL windows (presentation and notes) based on the intrinsic PDF
 * dimensions.
 *
 * @window_left: Output pointer for the left window.
 * @window_right: Output pointer for the right window.
 * @pdf_width: The intrinsic PDF width.
 * @pdf_height: The intrinsic PDF height.
 */
static int create_windows(SDL_Window **window_left, SDL_Window **window_right,
                          double pdf_width, double pdf_height) {
    int init_scale = 1;
    int init_img_width = (int)(pdf_width * init_scale);
    int init_img_height = (int)(pdf_height * init_scale);
    int init_left_width = init_img_width / 2;
    int init_right_width = init_img_width - init_left_width;

    *window_left = create_window("Left Half", init_left_width, init_img_height);
    *window_right =
        create_window("Right Half", init_right_width, init_img_height);

    if (!*window_left || !*window_right) {
        fprintf(stderr, "Failed to create SDL windows: %s\n", SDL_GetError());
        if (*window_left)
            SDL_DestroyWindow(*window_left);
        if (*window_right)
            SDL_DestroyWindow(*window_right);
        return -EIO;
    }

    return 0;
}

/**
 * Create SDL renderers for the provided windows.
 *
 * @renderer_left: Output pointer for the left renderer.
 * @renderer_right: Output pointer for the right renderer.
 * @window_left: The left SDL window.
 * @window_right: The right SDL window.
 */
static int create_renderers(SDL_Renderer **renderer_left,
                            SDL_Renderer **renderer_right,
                            SDL_Window *window_left, SDL_Window *window_right) {
    *renderer_left = SDL_CreateRenderer(
        window_left, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    *renderer_right = SDL_CreateRenderer(
        window_right, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);

    if (!*renderer_left || !*renderer_right) {
        fprintf(stderr, "Failed to create SDL renderers: %s\n", SDL_GetError());
        if (*renderer_left)
            SDL_DestroyRenderer(*renderer_left);
        if (*renderer_right)
            SDL_DestroyRenderer(*renderer_right);
        return -EIO;
    }
    return 0;
}

/**
 * Handle SDL events in a loop, including key and window events, and render page
 * changes.
 *
 * @state: Pointer to the program state.
 * @num_pages: The total number of pages in the PDF.
 * @window_left: The left SDL window.
 * @window_right: The right SDL window.
 * @renderer_left: The left SDL renderer.
 * @renderer_right: The right SDL renderer.
 */
static int handle_sdl_events(struct prog_state *state, int num_pages,
                             SDL_Window *window_left, SDL_Window *window_right,
                             SDL_Renderer *renderer_left,
                             SDL_Renderer *renderer_right) {
    int running = 1;
    int pending_quit = 0;
    int current_page = 0;
    Uint32 window_left_id = SDL_GetWindowID(window_left);
    Uint32 window_right_id = SDL_GetWindowID(window_right);
    Uint32 last_activity = SDL_GetTicks();

    struct render_cache cache = {0};
    init_cache_for_page(&cache, 0, state, renderer_left, renderer_right);
    if (cache.cur) {
        apply_cache_entry_to_state(state, cache.cur);
        present_texture(renderer_left, state->left.texture,
                        state->left.natural_width, state->left.natural_height);
        present_texture(renderer_right, state->right.texture,
                        state->right.natural_width,
                        state->right.natural_height);
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
                            update_cache_on_page_change(&cache, current_page,
                                                        state, renderer_left,
                                                        renderer_right);
                            if (cache.cur) {
                                apply_cache_entry_to_state(state, cache.cur);
                                present_texture(renderer_left,
                                                state->left.texture,
                                                state->left.natural_width,
                                                state->left.natural_height);
                                present_texture(renderer_right,
                                                state->right.texture,
                                                state->right.natural_width,
                                                state->right.natural_height);
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
                            compute_scale(window_left, window_right,
                                          state->pdf_width, state->pdf_height);
                        if (fabs(new_scale - state->current_scale) > 0.01) {
                            state->current_scale = new_scale;
                            fprintf(stderr, "Window resized, new scale: %.2f\n",
                                    new_scale);
                            init_cache_for_page(&cache, current_page, state,
                                                renderer_left, renderer_right);
                            if (cache.cur) {
                                apply_cache_entry_to_state(state, cache.cur);
                                present_texture(renderer_left,
                                                state->left.texture,
                                                state->left.natural_width,
                                                state->left.natural_height);
                                present_texture(renderer_right,
                                                state->right.texture,
                                                state->right.natural_width,
                                                state->right.natural_height);
                            }
                        }
                    } else if (event.window.event == SDL_WINDOWEVENT_EXPOSED ||
                               event.window.event == SDL_WINDOWEVENT_SHOWN ||
                               event.window.event == SDL_WINDOWEVENT_RESTORED) {
                        if (event.window.windowID == window_left_id)
                            present_texture(renderer_left, state->left.texture,
                                            state->left.natural_width,
                                            state->left.natural_height);
                        else if (event.window.windowID == window_right_id)
                            present_texture(renderer_right,
                                            state->right.texture,
                                            state->right.natural_width,
                                            state->right.natural_height);
                    }
                    break;
                default:
                    break;
            }
        }

        if (SDL_GetTicks() - last_activity > 50) {
            update_cache(&cache, current_page, state, renderer_left,
                         renderer_right, num_pages);
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

    _drop_(SDL_DestroyWindow) SDL_Window *window_left = NULL;
    _drop_(SDL_DestroyWindow) SDL_Window *window_right = NULL;
    ret = create_windows(&window_left, &window_right, ps.pdf_width,
                         ps.pdf_height);
    if (ret < 0) {
        SDL_Quit();
        return EXIT_FAILURE;
    }

    _drop_(SDL_DestroyRenderer) SDL_Renderer *renderer_left = NULL;
    _drop_(SDL_DestroyRenderer) SDL_Renderer *renderer_right = NULL;
    ret = create_renderers(&renderer_left, &renderer_right, window_left,
                           window_right);
    if (ret < 0) {
        SDL_Quit();
        return EXIT_FAILURE;
    }

    ps.current_scale =
        compute_scale(window_left, window_right, ps.pdf_width, ps.pdf_height);

    int num_pages = poppler_document_get_n_pages(ps.document);
    ret = handle_sdl_events(&ps, num_pages, window_left, window_right,
                            renderer_left, renderer_right);
    if (ret < 0) {
        SDL_Quit();
        return EXIT_FAILURE;
    }

    if (ps.document) {
        g_object_unref(ps.document);
        ps.document = NULL;
    }

    SDL_Quit();
    return EXIT_SUCCESS;
}
