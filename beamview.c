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

#define expect(x)                                                              \
    do {                                                                       \
        if (!(x)) {                                                            \
            fprintf(stderr, "!(%s) at %s:%s:%d\n", #x, __FILE__, __func__,     \
                    __LINE__);                                                 \
            abort();                                                           \
        }                                                                      \
    } while (0)
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

enum split_orientation {
    SPLIT_HORIZONTAL, // left/right split
    SPLIT_VERTICAL    // top/bottom split
};

struct texture_data {
    GLuint texture;
    int natural_width;
    int natural_height;
};

struct gl_ctx {
    GLFWwindow *window;
    struct texture_data texture;
};

struct render_cache_entry {
    int page_index;
    GLuint textures[NUM_CONTEXTS];
    int widths[NUM_CONTEXTS];
    int texture_height;
};

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
    enum split_orientation orientation;
};

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

    GLfloat vertices[] = {(GLfloat)dst_x,
                          (GLfloat)dst_y,
                          (GLfloat)(dst_x + new_width),
                          (GLfloat)dst_y,
                          (GLfloat)(dst_x + new_width),
                          (GLfloat)(dst_y + new_height),
                          (GLfloat)dst_x,
                          (GLfloat)(dst_y + new_height)};

    GLfloat tex_coords[] = {0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};

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

static double compute_scale(struct gl_ctx ctx[], int num_ctx, double pdf_width,
                            double pdf_height,
                            enum split_orientation orientation) {
    if (pdf_width <= 0 || pdf_height <= 0) {
        fprintf(stderr, "Invalid PDF dimensions: width=%.2f, height=%.2f\n",
                pdf_width, pdf_height);
        return 1.0;
    }
    double scale = 0;
    for (int i = 0; i < num_ctx; i++) {
        int win_width, win_height;
        glfwGetFramebufferSize(ctx[i].window, &win_width, &win_height);
        double scale_i;
        if (orientation == SPLIT_HORIZONTAL) {
            scale_i = fmax((double)win_width / (pdf_width / num_ctx),
                           (double)win_height / pdf_height);
        } else { // SPLIT_VERTICAL
            scale_i = fmax((double)win_width / pdf_width,
                           (double)win_height / (pdf_height / num_ctx));
        }
        if (scale_i > scale)
            scale = scale_i;
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

static GLuint create_gl_texture_from_cairo_region(
    cairo_surface_t *cairo_surface, enum split_orientation orientation,
    int offset, int region_size, int full_width, int full_height) {
    int cairo_width = cairo_image_surface_get_width(cairo_surface);
    int cairo_height = cairo_image_surface_get_height(cairo_surface);
    int cairo_stride = cairo_image_surface_get_stride(cairo_surface);
    unsigned char *cairo_data = cairo_image_surface_get_data(cairo_surface);

    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    if (orientation == SPLIT_HORIZONTAL) {
        if (offset < 0 || region_size <= 0 ||
            offset + region_size > cairo_width) {
            fprintf(
                stderr,
                "Requested region exceeds cairo surface bounds: offset %d, region_size %d, cairo_width %d\n",
                offset, region_size, cairo_width);
            return 0;
        }
        int row_length = cairo_stride / 4;
        glPixelStorei(GL_UNPACK_ROW_LENGTH, row_length);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, region_size, full_height, 0,
                     GL_BGRA, GL_UNSIGNED_BYTE, cairo_data + offset * 4);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    } else { // SPLIT_VERTICAL
        if (offset < 0 || region_size <= 0 ||
            offset + region_size > cairo_height) {
            fprintf(
                stderr,
                "Requested region exceeds cairo surface bounds: offset %d, region_size %d, cairo_height %d\n",
                offset, region_size, cairo_height);
            return 0;
        }
        int row_length = cairo_stride / 4;
        glPixelStorei(GL_UNPACK_ROW_LENGTH, row_length);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, full_width, region_size, 0,
                     GL_BGRA, GL_UNSIGNED_BYTE,
                     cairo_data + offset * cairo_stride);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    }
    return texture;
}

static void free_cache_entry(struct render_cache_entry *entry) {
    if (!entry)
        return;
    for (int i = 0; i < NUM_CONTEXTS; i++) {
        if (entry->textures[i])
            glDeleteTextures(1, &entry->textures[i]);
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

    struct render_cache_entry *entry =
        malloc(sizeof(struct render_cache_entry));
    expect(entry);
    entry->page_index = page_index;

    if (state->orientation == SPLIT_HORIZONTAL) {
        entry->texture_height = img_height;
        int base_width = img_width / state->num_ctx;
        for (int i = 0; i < state->num_ctx; i++) {
            int x_offset = i * base_width;
            int region_width = (i == state->num_ctx - 1)
                                   ? (img_width - base_width * i)
                                   : base_width;
            entry->widths[i] = region_width;
            entry->textures[i] = create_gl_texture_from_cairo_region(
                cairo_surface, state->orientation, x_offset, region_width,
                img_width, img_height);
            if (!entry->textures[i]) {
                fprintf(
                    stderr,
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
    } else { // SPLIT_VERTICAL
        int base_height = img_height / state->num_ctx;
        for (int i = 0; i < state->num_ctx; i++) {
            int y_offset = i * base_height;
            int region_height = (i == state->num_ctx - 1)
                                    ? (img_height - base_height * i)
                                    : base_height;
            entry->widths[i] = region_height;
            entry->textures[i] = create_gl_texture_from_cairo_region(
                cairo_surface, state->orientation, y_offset, region_height,
                img_width, img_height);
            if (!entry->textures[i]) {
                fprintf(
                    stderr,
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
        entry->texture_height = img_width;
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

static int create_contexts(struct gl_ctx ctx[], int num_ctx, double pdf_width,
                           double pdf_height,
                           enum split_orientation orientation) {
    int total, init_other;
    if (orientation == SPLIT_HORIZONTAL) {
        total = (int)pdf_width;
        init_other = (int)pdf_height;
    } else {
        total = (int)pdf_height;
        init_other = (int)pdf_width;
    }
    int sizes[num_ctx];
    int base = total / num_ctx;
    for (int i = 0; i < num_ctx; i++) {
        sizes[i] = base;
    }
    sizes[num_ctx - 1] = total - base * (num_ctx - 1);

    char title[32];
    if (orientation == SPLIT_HORIZONTAL) {
        snprintf(title, sizeof(title), "Context %d", 0);
        ctx[0].window =
            glfwCreateWindow(sizes[0], init_other, title, NULL, NULL);
    } else {
        snprintf(title, sizeof(title), "Context %d", 0);
        ctx[0].window =
            glfwCreateWindow(init_other, sizes[0], title, NULL, NULL);
    }
    if (!ctx[0].window) {
        fprintf(stderr, "Failed to create GLFW window for context 0\n");
        return -EIO;
    }
    for (int i = 1; i < num_ctx; i++) {
        snprintf(title, sizeof(title), "Context %d", i);
        if (orientation == SPLIT_HORIZONTAL)
            ctx[i].window = glfwCreateWindow(sizes[i], init_other, title, NULL,
                                             ctx[0].window);
        else
            ctx[i].window = glfwCreateWindow(init_other, sizes[i], title, NULL,
                                             ctx[0].window);
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
                if (state->cache_entries[new_page])
                    state->current_page = new_page;
                else
                    fprintf(stderr, "Error rendering page %d\n", new_page);
            }
        }
    }
}

static void framebuffer_size_callback(GLFWwindow *window, int width,
                                      int height) {
    (void)width;
    (void)height;
    struct prog_state *state =
        (struct prog_state *)glfwGetWindowUserPointer(window);
    double new_scale =
        compute_scale(state->ctx, state->num_ctx, state->pdf_width,
                      state->pdf_height, state->orientation);
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

static int handle_glfw_events(struct prog_state *state) {
    for (int i = 0; i < state->num_ctx; i++) {
        glfwSetKeyCallback(state->ctx[i].window, key_callback);
        glfwSetFramebufferSizeCallback(state->ctx[i].window,
                                       framebuffer_size_callback);
        glfwSetWindowUserPointer(state->ctx[i].window, state);
    }

    glfwMakeContextCurrent(state->ctx[0].window);

    /* Render the first page (blocking render); others will be cached. */
    state->cache_entries[state->current_page] =
        create_cache_entry(state->current_page, state);
    if (state->cache_entries[state->current_page]) {
        for (int i = 0; i < state->num_ctx; i++) {
            glfwMakeContextCurrent(state->ctx[i].window);
            if (state->orientation == SPLIT_HORIZONTAL) {
                present_texture(
                    state->ctx[i].window,
                    state->cache_entries[state->current_page]->textures[i],
                    state->cache_entries[state->current_page]->widths[i],
                    state->cache_entries[state->current_page]->texture_height);
            } else {
                present_texture(
                    state->ctx[i].window,
                    state->cache_entries[state->current_page]->textures[i],
                    state->cache_entries[state->current_page]->texture_height,
                    state->cache_entries[state->current_page]->widths[i]);
            }
        }
    }

    while (!glfwWindowShouldClose(state->ctx[0].window)) {
        if (state->cache_complete)
            glfwWaitEvents();
        else {
            glfwWaitEventsTimeout(0.01);
            cache_one_slide(state->cache_entries, state->num_pages, state);
        }
        if (state->cache_entries[state->current_page]) {
            for (int i = 0; i < state->num_ctx; i++) {
                glfwMakeContextCurrent(state->ctx[i].window);
                if (state->orientation == SPLIT_HORIZONTAL) {
                    present_texture(
                        state->ctx[i].window,
                        state->cache_entries[state->current_page]->textures[i],
                        state->cache_entries[state->current_page]->widths[i],
                        state->cache_entries[state->current_page]
                            ->texture_height);
                } else {
                    present_texture(
                        state->ctx[i].window,
                        state->cache_entries[state->current_page]->textures[i],
                        state->cache_entries[state->current_page]
                            ->texture_height,
                        state->cache_entries[state->current_page]->widths[i]);
                }
            }
        }
    }
    free_all_cache_entries(state->cache_entries, state->num_pages);
    free(state->cache_entries);
    return 0;
}

int main(int argc, char *argv[]) {
    const char *pdf_file = NULL;
    enum split_orientation orientation = SPLIT_HORIZONTAL;

    if (argc < 2) {
        fprintf(stderr, "Usage: %s [-h|-v] <pdf_file>\n", argv[0]);
        return EXIT_FAILURE;
    }
    if (argc == 3) {
        if (strcmp(argv[1], "-h") == 0)
            orientation = SPLIT_HORIZONTAL;
        else if (strcmp(argv[1], "-v") == 0)
            orientation = SPLIT_VERTICAL;
        else {
            fprintf(stderr, "Unknown option %s\n", argv[1]);
            return EXIT_FAILURE;
        }
        pdf_file = argv[2];
    } else {
        pdf_file = argv[1];
    }

    expect(glfwInit());
    struct prog_state ps;
    expect(init_prog_state(&ps, pdf_file) == 0);
    ps.orientation = orientation;
    ps.num_ctx = NUM_CONTEXTS;
    ps.current_page = 0;
    ps.pending_quit = 0;
    ps.num_pages = poppler_document_get_n_pages(ps.document);
    ps.cache_entries =
        calloc(ps.num_pages, sizeof(struct render_cache_entry *));
    expect(ps.cache_entries);
    ps.ctx = calloc(ps.num_ctx, sizeof(struct gl_ctx));
    expect(ps.ctx);
    expect(create_contexts(ps.ctx, ps.num_ctx, ps.pdf_width, ps.pdf_height,
                           ps.orientation) == 0);

    ps.current_scale = compute_scale(ps.ctx, ps.num_ctx, ps.pdf_width,
                                     ps.pdf_height, ps.orientation);

    expect(handle_glfw_events(&ps) == 0);

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
