#include <GL/gl.h>
#include <GLFW/glfw3.h>
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

#define DEFINE_DROP_FUNC_VOID(func)                                            \
    static inline void drop_##func(void *p) {                                  \
        void **pp = p;                                                         \
        if (*pp)                                                               \
            func(*pp);                                                         \
    }

DEFINE_DROP_FUNC_VOID(free)
DEFINE_DROP_FUNC(cairo_surface_t *, cairo_surface_destroy)
DEFINE_DROP_FUNC_COERCE(GObject *, g_object_unref)

#define NUM_CONTEXTS 2

/**
 * Holds texture information and its natural dimensions.
 *
 * @texture: The OpenGL texture.
 * @natural_width: The original width of the texture.
 * @natural_height: The original height of the texture.
 */
struct texture_data {
    GLuint texture;
    int natural_width;
    int natural_height;
};

/* Represents a context, including a GLFW window and texture data. */
struct gl_ctx {
    GLFWwindow *window;
    struct texture_data texture;
};

/**
 * Represents a cached rendered page.
 *
 * @page_index: The page index corresponding to this entry.
 * @textures: Array of textures for each context.
 * @widths: Array of natural widths for each texture.
 * @texture_height: Natural height of the combined page.
 */
struct render_cache_entry {
    int page_index;
    GLuint textures[NUM_CONTEXTS];
    int widths[NUM_CONTEXTS];
    int texture_height;
};

/**
 * Holds the program state including contexts, PDF dimensions, scale, and
 * document.
 *
 * @ctx: Array of contexts.
 * @num_ctx: Number of contexts.
 * @pdf_width: The intrinsic PDF width.
 * @pdf_height: The intrinsic PDF height.
 * @current_scale: The scale factor applied for rendering.
 * @document: The PopplerDocument representing the PDF.
 * @cache_complete: Flag to bypass recaching if all slides are cached.
 * @current_page: The currently displayed page.
 * @pending_quit: Flag to allow fast exit on repeated Q key presses.
 * @num_pages: Total number of pages in the document.
 * @cache_entries: Array of cached rendered pages.
 */
struct prog_state {
    struct gl_ctx *ctx;
    int num_ctx;
    double pdf_width;
    double pdf_height;
    double current_scale;
    PopplerDocument *document;
    int cache_complete;
    int current_page;
    int pending_quit;
    int num_pages;
    struct render_cache_entry **cache_entries;
};

/**
 * Present a texture on a window while maintaining its aspect ratio.
 *
 * @window: The GLFW window with an OpenGL context.
 * @texture: The OpenGL texture to present.
 * @natural_width: The original width of the texture.
 * @natural_height: The original height of the texture.
 */
static void present_texture(GLFWwindow *window, GLuint texture,
                            int natural_width, int natural_height) {
    if (!texture)
        return;

    int win_width, win_height;
    glfwGetFramebufferSize(window, &win_width, &win_height);

    float scale = fmin((float)win_width / natural_width,
                       (float)win_height / natural_height);
    int new_width = (int)(natural_width * scale);
    int new_height = (int)(natural_height * scale);

    int dst_x = (win_width - new_width) / 2;
    int dst_y = (win_height - new_height) / 2;

    glfwMakeContextCurrent(window);
    glViewport(0, 0, win_width, win_height);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, win_width, win_height, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, texture);

    GLfloat vertices[] = {
        (GLfloat)dst_x, (GLfloat)dst_y,
        (GLfloat)(dst_x + new_width), (GLfloat)dst_y,
        (GLfloat)(dst_x + new_width), (GLfloat)(dst_y + new_height),
        (GLfloat)dst_x, (GLfloat)(dst_y + new_height)
    };

    GLfloat tex_coords[] = {
        0.0f, 0.0f,
        1.0f, 0.0f,
        1.0f, 1.0f,
        0.0f, 1.0f
    };

    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glVertexPointer(2, GL_FLOAT, 0, vertices);
    glTexCoordPointer(2, GL_FLOAT, 0, tex_coords);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);

    glDisable(GL_TEXTURE_2D);
    glfwSwapBuffers(window);
}

/**
 * Compute the required scale factor based on the intrinsic PDF dimensions and
 * the sizes of windows in multiple contexts.
 *
 * @ctx: Array of contexts.
 * @num_ctx: Number of contexts.
 * @pdf_width: The intrinsic PDF width.
 * @pdf_height: The intrinsic PDF height.
 */
static double compute_scale(struct gl_ctx ctx[], int num_ctx, double pdf_width,
                            double pdf_height) {
    if (pdf_width <= 0 || pdf_height <= 0) {
        fprintf(stderr, "Invalid PDF dimensions: width=%.2f, height=%.2f\n",
                pdf_width, pdf_height);
        return 1.0;
    }
    double scale = 0;
    for (int i = 0; i < num_ctx; i++) {
        int win_width, win_height;
        glfwGetFramebufferSize(ctx[i].window, &win_width, &win_height);
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
 * Create an OpenGL texture from a region of a Cairo image surface.
 *
 * @cairo_surface: The source Cairo surface.
 * @x_offset: The starting x offset in pixels.
 * @region_width: The width of the region to extract.
 * @img_height: The height of the image.
 */
static GLuint create_gl_texture_from_cairo(cairo_surface_t *cairo_surface,
                                           int x_offset, int region_width,
                                           int img_height) {
    int cairo_width = cairo_image_surface_get_width(cairo_surface);
    if (x_offset < 0 || region_width <= 0 ||
        x_offset + region_width > cairo_width) {
        fprintf(
            stderr,
            "Requested region exceeds cairo surface bounds: x_offset %d, region_width %d, cairo_width %d\n",
            x_offset, region_width, cairo_width);
        return 0;
    }

    int cairo_stride = cairo_image_surface_get_stride(cairo_surface);
    unsigned char *cairo_data = cairo_image_surface_get_data(cairo_surface);

    _drop_(free) unsigned char *buffer = malloc(region_width * img_height * 4);
    if (!buffer) {
        fprintf(stderr, "Failed to allocate buffer for texture\n");
        return 0;
    }
    for (int y = 0; y < img_height; y++) {
        unsigned char *src_row = cairo_data + y * cairo_stride + x_offset * 4;
        memcpy(buffer + y * region_width * 4, src_row, region_width * 4);
    }

    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, region_width, img_height, 0,
                 GL_BGRA, GL_UNSIGNED_BYTE, buffer);
    return texture;
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
            glDeleteTextures(1, &entry->textures[i]);
    }
    free(entry);
}

/**
 * Render and create a cache entry for a given page index.
 *
 * Added debug output to indicate when a page is rendered live.
 *
 * @page_index: The index of the page to render.
 * @state: Pointer to the program state.
 *
 * @return A new render_cache_entry for the requested page, or NULL on failure.
 */
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

    struct render_cache_entry *entry =
        malloc(sizeof(struct render_cache_entry));
    if (!entry) {
        return NULL;
    }
    entry->page_index = page_index;
    entry->texture_height = img_height;
    int base_width = img_width / state->num_ctx;
    for (int i = 0; i < state->num_ctx; i++) {
        int x_offset = i * base_width;
        int region_width = (i == state->num_ctx - 1)
                               ? (img_width - base_width * i)
                               : base_width;
        entry->widths[i] = region_width;
        entry->textures[i] = create_gl_texture_from_cairo(
            cairo_surface, x_offset, region_width, img_height);
        if (!entry->textures[i]) {
            fprintf(stderr,
                    "Failed to create OpenGL texture for page %d, context %d\n",
                    page_index, i);
            for (int j = 0; j < i; j++) {
                if (entry->textures[j])
                    glDeleteTextures(1, &entry->textures[j]);
            }
            free(entry);
            return NULL;
        }
    }
    fprintf(stderr, "Cache page %d created.\n", page_index);
    return entry;
}

/**
 * Free all cache entries in the provided cache array.
 *
 * @cache_entries: Array of cache entries.
 * @num_pages: Total number of pages.
 */
static void free_all_cache_entries(struct render_cache_entry **cache_entries,
                                   int num_pages) {
    for (int i = 0; i < num_pages; i++) {
        if (cache_entries[i]) {
            free_cache_entry(cache_entries[i]);
            cache_entries[i] = NULL;
        }
    }
}

/**
 * When idle (i.e., no events for 50ms), update the cache by creating any
 * missing cache entries.
 *
 * Renders one missing page per idle cycle.
 *
 * Added fast exit mode if all slides are already cached.
 *
 * @cache_entries: Array of cache entries.
 * @num_pages: The total number of pages in the document.
 * @state: Pointer to the program state.
 */
static void cache_one_slide(struct render_cache_entry **cache_entries,
                            int num_pages, struct prog_state *state) {
    if (state->cache_complete)
        return;

    int complete = 1;
    for (int i = 0; i < num_pages; i++) {
        if (!cache_entries[i]) {
            cache_entries[i] = create_cache_entry(i, state);
            complete = 0;
            // Render one missing slide per idle cycle.
            break;
        }
    }
    if (complete == 1) {
        fprintf(stderr, "Caching complete.\n");
    }
    state->cache_complete = complete;
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
    state->cache_complete = 0;

    return 0;
}

/**
 * Create a window with the given title and dimensions.
 *
 * @title: The window title.
 * @width: The window width.
 * @height: The window height.
 * @share: An existing window to share the OpenGL context with (can be NULL).
 */
static GLFWwindow *create_window(const char *title, int width, int height,
                                 GLFWwindow *share) {
    return glfwCreateWindow(width, height, title, NULL, share);
}

/**
 * Create contexts (windows) based on the intrinsic PDF dimensions.
 *
 * @ctx: Array of contexts to populate.
 * @num_ctx: Number of contexts.
 * @pdf_width: The intrinsic PDF width.
 * @pdf_height: The intrinsic PDF height.
 */
static int create_contexts(struct gl_ctx ctx[], int num_ctx, double pdf_width,
                           double pdf_height) {
    int init_img_width = (int)pdf_width;
    int init_img_height = (int)pdf_height;
    int widths[num_ctx];
    for (int i = 0; i < num_ctx; i++) {
        widths[i] = init_img_width / num_ctx;
    }
    widths[num_ctx - 1] =
        init_img_width - (init_img_width / num_ctx) * (num_ctx - 1);

    /* Create the first window normally */
    char title[32];
    snprintf(title, sizeof(title), "Context %d", 0);
    ctx[0].window = create_window(title, widths[0], init_img_height, NULL);
    if (!ctx[0].window) {
        fprintf(stderr, "Failed to create GLFW window for context 0\n");
        return -EIO;
    }
    /* Create remaining windows sharing the first window's context */
    for (int i = 1; i < num_ctx; i++) {
        snprintf(title, sizeof(title), "Context %d", i);
        ctx[i].window =
            create_window(title, widths[i], init_img_height, ctx[0].window);
        if (!ctx[i].window) {
            fprintf(stderr, "Failed to create GLFW window for context %d\n", i);
            for (int j = 0; j < i; j++) {
                if (ctx[j].window)
                    glfwDestroyWindow(ctx[j].window);
            }
            return -EIO;
        }
    }
    return 0;
}

/**
 * Create renderers for the provided contexts.
 *
 * Note: With GLFW the window’s OpenGL context is used, so no separate renderer
 * is needed.
 */
static int create_context_renderers(struct gl_ctx ctx[], int num_ctx) {
    (void)ctx;
    (void)num_ctx;
    return 0;
}

static void key_callback(GLFWwindow *window, int key, int scancode, int action,
                         int mods) {
    (void)scancode;
    (void)mods;
    if (action == GLFW_PRESS) {
        struct prog_state *state =
            (struct prog_state *)glfwGetWindowUserPointer(window);
        if (key == GLFW_KEY_Q) {
            if (state->pending_quit) {
                for (int i = 0; i < state->num_ctx; i++) {
                    glfwSetWindowShouldClose(state->ctx[i].window, GLFW_TRUE);
                }
            } else {
                state->pending_quit = 1;
            }
        } else {
            state->pending_quit = 0;
            int new_page = state->current_page;
            if (key == GLFW_KEY_LEFT || key == GLFW_KEY_UP ||
                key == GLFW_KEY_PAGE_UP) {
                if (state->current_page > 0)
                    new_page = state->current_page - 1;
            } else if (key == GLFW_KEY_RIGHT || key == GLFW_KEY_DOWN ||
                       key == GLFW_KEY_PAGE_DOWN) {
                if (state->current_page < state->num_pages - 1)
                    new_page = state->current_page + 1;
            }
            if (new_page != state->current_page) {
                if (state->cache_entries[new_page] == NULL) {
                    fprintf(
                        stderr,
                        "Warning: Page %d not cached; performing blocking render.\n",
                        new_page);
                    state->cache_entries[new_page] =
                        create_cache_entry(new_page, state);
                }
                if (state->cache_entries[new_page]) {
                    state->current_page = new_page;
                } else {
                    fprintf(stderr, "Error rendering page %d\n", new_page);
                }
            }
        }
    }
}

/**
 * Framebuffer size callback to handle window resize events.
 */
static void framebuffer_size_callback(GLFWwindow *window, int width,
                                      int height) {
    (void)width;
    (void)height;
    struct prog_state *state =
        (struct prog_state *)glfwGetWindowUserPointer(window);
    double new_scale = compute_scale(state->ctx, state->num_ctx,
                                     state->pdf_width, state->pdf_height);
    if (fabs(new_scale - state->current_scale) > 0.01) {
        state->current_scale = new_scale;
        fprintf(stderr, "Window resized, new scale: %.2f\n", new_scale);
        free_all_cache_entries(state->cache_entries, state->num_pages);
        state->cache_complete = 0;
        fprintf(stderr, "Cache invalidated due to window resize.\n");
        state->cache_entries[state->current_page] =
            create_cache_entry(state->current_page, state);
    }
}

/**
 * Handle GLFW events in a loop, including key and window events, and render
 * page changes.
 *
 * @state: Pointer to the program state.
 */
static int handle_glfw_events(struct prog_state *state) {
    for (int i = 0; i < state->num_ctx; i++) {
        glfwSetKeyCallback(state->ctx[i].window, key_callback);
        glfwSetFramebufferSizeCallback(state->ctx[i].window,
                                       framebuffer_size_callback);
        glfwSetWindowUserPointer(state->ctx[i].window, state);
    }

    glfwMakeContextCurrent(state->ctx[0].window);

    // The first page is rendered blocking, the rest are cached
    state->cache_entries[state->current_page] =
        create_cache_entry(state->current_page, state);
    if (state->cache_entries[state->current_page]) {
        for (int i = 0; i < state->num_ctx; i++) {
            glfwMakeContextCurrent(state->ctx[i].window);
            present_texture(
                state->ctx[i].window,
                state->cache_entries[state->current_page]->textures[i],
                state->cache_entries[state->current_page]->widths[i],
                state->cache_entries[state->current_page]->texture_height);
        }
    }

    while (!glfwWindowShouldClose(state->ctx[0].window)) {
        if (state->cache_complete) {
            glfwWaitEvents();
        } else {
            // Allow a keypress to interrupt caching
            glfwWaitEventsTimeout(0.01);
            cache_one_slide(state->cache_entries, state->num_pages, state);
        }

        if (state->cache_entries[state->current_page]) {
            for (int i = 0; i < state->num_ctx; i++) {
                glfwMakeContextCurrent(state->ctx[i].window);
                present_texture(
                    state->ctx[i].window,
                    state->cache_entries[state->current_page]->textures[i],
                    state->cache_entries[state->current_page]->widths[i],
                    state->cache_entries[state->current_page]->texture_height);
            }
        }
    }
    free_all_cache_entries(state->cache_entries, state->num_pages);
    free(state->cache_entries);
    return 0;
}

int main(int argc, char *argv[]) {
    int ret = 0;

    if (argc < 2) {
        fprintf(stderr, "Usage: %s <pdf_file>\n", argv[0]);
        return EXIT_FAILURE;
    }

    if (!glfwInit()) {
        fprintf(stderr, "GLFW could not initialise\n");
        return EXIT_FAILURE;
    }

    struct prog_state ps;
    ret = init_prog_state(&ps, argv[1]);
    if (ret < 0) {
        glfwTerminate();
        return EXIT_FAILURE;
    }

    ps.num_ctx = NUM_CONTEXTS;
    ps.current_page = 0;
    ps.pending_quit = 0;
    ps.num_pages = poppler_document_get_n_pages(ps.document);
    ps.cache_entries =
        calloc(ps.num_pages, sizeof(struct render_cache_entry *));
    if (!ps.cache_entries) {
        fprintf(stderr, "Failed to allocate cache entries array.\n");
        if (ps.document) {
            g_object_unref(ps.document);
            ps.document = NULL;
        }
        glfwTerminate();
        return EXIT_FAILURE;
    }

    ps.ctx = calloc(ps.num_ctx, sizeof(struct gl_ctx));
    if (!ps.ctx) {
        fprintf(stderr, "Failed to allocate contexts.\n");
        if (ps.document) {
            g_object_unref(ps.document);
            ps.document = NULL;
        }
        free(ps.cache_entries);
        glfwTerminate();
        return EXIT_FAILURE;
    }

    ret = create_contexts(ps.ctx, ps.num_ctx, ps.pdf_width, ps.pdf_height);
    if (ret < 0) {
        free(ps.ctx);
        glfwTerminate();
        return EXIT_FAILURE;
    }

    ret = create_context_renderers(ps.ctx, ps.num_ctx);
    if (ret < 0) {
        for (int i = 0; i < ps.num_ctx; i++) {
            if (ps.ctx[i].window)
                glfwDestroyWindow(ps.ctx[i].window);
        }
        free(ps.ctx);
        glfwTerminate();
        return EXIT_FAILURE;
    }

    ps.current_scale =
        compute_scale(ps.ctx, ps.num_ctx, ps.pdf_width, ps.pdf_height);

    ret = handle_glfw_events(&ps);
    if (ret < 0) {
        glfwTerminate();
        return EXIT_FAILURE;
    }

    if (ps.document) {
        g_object_unref(ps.document);
        ps.document = NULL;
    }

    for (int i = 0; i < ps.num_ctx; i++) {
        if (ps.ctx[i].window)
            glfwDestroyWindow(ps.ctx[i].window);
    }
    free(ps.ctx);
    glfwTerminate();
    return EXIT_SUCCESS;
}
