#include <GL/gl.h>
#include <GLFW/glfw3.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <mupdf/fitz.h>
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

#define NUM_CONTEXTS 2

struct texture_data {
    GLuint texture;
    int natural_width, natural_height;
};

struct gl_ctx {
    GLFWwindow *window;
    struct texture_data texture;
    int is_fullscreen;
    int windowed_x, windowed_y, windowed_width, windowed_height;
};

struct render_cache_entry {
    int page_index;
    GLuint textures[NUM_CONTEXTS];
    int widths[NUM_CONTEXTS], texture_height;
};

struct prog_state {
    struct gl_ctx *ctx;
    int num_ctx;
    double pdf_width, pdf_height, current_scale;
    fz_document *document;
    fz_context *fz_ctx;
    int cache_complete, current_page, num_pages;
    struct render_cache_entry **cache_entries;

    // You might wonder why we need this and we don't just unconditionally
    // redraw after each GLFW event. That works fine on X11, but on Wayland it
    // burns up the CPU because the buffer swap itself causes an event. See
    // https://github.com/glfw/glfw/issues/1911.
    int needs_redraw;
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

    glfwMakeContextCurrent(window);
    glViewport(0, 0, win_width, win_height);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, win_width, win_height, 0, -1, 1);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, texture);

    const GLfloat left = (GLfloat)((win_width - new_width) / 2);
    const GLfloat top = (GLfloat)((win_height - new_height) / 2);
    const GLfloat right = left + new_width;
    const GLfloat bottom = top + new_height;
    GLfloat vertices[] = {left, top, right, top, right, bottom, left, bottom};
    GLfloat tex_coords[] = {0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};

    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glVertexPointer(2, GL_FLOAT, 0, vertices);
    glTexCoordPointer(2, GL_FLOAT, 0, tex_coords);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    glfwSwapBuffers(window);
}

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
        double scale_i = fmin((double)win_width / (pdf_width / num_ctx),
                              (double)win_height / pdf_height);
        scale = fmax(scale, scale_i);
    }
    return scale;
}

static fz_pixmap *render_page_to_pixmap(int page_index,
                                        struct prog_state *state,
                                        int *img_width, int *img_height) {
    fz_matrix ctm = fz_scale(state->current_scale, state->current_scale);
    fz_pixmap *pix = fz_new_pixmap_from_page_number(
        state->fz_ctx, state->document, page_index, ctm,
        fz_device_rgb(state->fz_ctx), 0);
    expect(pix);
    *img_width = pix->w;
    *img_height = pix->h;
    return pix;
}

static GLuint create_gl_texture_from_pixmap_region(fz_pixmap *pix, int offset_x,
                                                   int region_width,
                                                   int full_height) {
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, pix->stride / pix->n);
    unsigned char *start_ptr = pix->samples + offset_x * pix->n;
    GLuint texture;
    glGenTextures(1, &texture);
    expect(texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    GLenum format = (pix->n == 4) ? GL_RGBA : GL_RGB;
    glTexImage2D(GL_TEXTURE_2D, 0, format, region_width, full_height, 0, format,
                 GL_UNSIGNED_BYTE, start_ptr);
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
    int img_width, img_height;
    fz_pixmap *pix =
        render_page_to_pixmap(page_index, state, &img_width, &img_height);
    expect(pix);

    struct render_cache_entry *entry =
        malloc(sizeof(struct render_cache_entry));
    expect(entry);
    entry->page_index = page_index;

    int base_split = img_width / state->num_ctx;
    entry->texture_height = img_height;

    for (int i = 0; i < state->num_ctx; i++) {
        int offset = i * base_split;
        int region_width = (i == state->num_ctx - 1)
                               ? (img_width - base_split * i)
                               : base_split;

        entry->widths[i] = region_width;
        entry->textures[i] = create_gl_texture_from_pixmap_region(
            pix, offset, region_width, img_height);
        expect(entry->textures[i]);
    }

    fprintf(stderr, "Cache page %d created.\n", page_index);
    fz_drop_pixmap(state->fz_ctx, pix);
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
    if (complete) {
        fprintf(stderr, "Caching complete.\n");
    }
    state->cache_complete = complete;
}

static int init_prog_state(struct prog_state *state, const char *pdf_file) {
    state->fz_ctx = fz_new_context(NULL, NULL, FZ_STORE_UNLIMITED);
    expect(state->fz_ctx);
    fz_register_document_handlers(state->fz_ctx);

    state->document = fz_open_document(state->fz_ctx, pdf_file);
    if (!state->document) {
        fprintf(stderr, "Error opening PDF.\n");
        fz_drop_context(state->fz_ctx);
        return -EIO;
    }

    int num_pages = fz_count_pages(state->fz_ctx, state->document);
    if (num_pages <= 0) {
        fprintf(stderr, "PDF has no pages.\n");
        fz_drop_document(state->fz_ctx, state->document);
        fz_drop_context(state->fz_ctx);
        return -EINVAL;
    }
    state->num_pages = num_pages;

    fz_page *first_page = fz_load_page(state->fz_ctx, state->document, 0);
    expect(first_page);
    fz_rect bounds = fz_bound_page(state->fz_ctx, first_page);
    state->pdf_width = bounds.x1 - bounds.x0;
    state->pdf_height = bounds.y1 - bounds.y0;
    fz_drop_page(state->fz_ctx, first_page);

    state->current_scale = 1.0;
    state->cache_complete = 0;

    return 0;
}

static GLFWmonitor *get_current_monitor(GLFWwindow *window) {
    int window_x, window_y, window_width, window_height;
    glfwGetWindowPos(window, &window_x, &window_y);
    glfwGetWindowSize(window, &window_width, &window_height);

    int window_center_x = window_x + window_width / 2;
    int window_center_y = window_y + window_height / 2;

    int monitor_count;
    GLFWmonitor **monitors = glfwGetMonitors(&monitor_count);

    if (!monitors || monitor_count == 0) {
        return glfwGetPrimaryMonitor();
    }

    for (int i = 0; i < monitor_count; i++) {
        int monitor_x, monitor_y;
        glfwGetMonitorPos(monitors[i], &monitor_x, &monitor_y);

        const GLFWvidmode *mode = glfwGetVideoMode(monitors[i]);
        if (!mode)
            continue;

        if (window_center_x >= monitor_x &&
            window_center_x < monitor_x + mode->width &&
            window_center_y >= monitor_y &&
            window_center_y < monitor_y + mode->height) {
            return monitors[i];
        }
    }

    return glfwGetPrimaryMonitor();
}

static void toggle_fullscreen(struct gl_ctx *ctx) {
    if (ctx->is_fullscreen) {
        glfwSetWindowMonitor(ctx->window, NULL, ctx->windowed_x,
                             ctx->windowed_y, ctx->windowed_width,
                             ctx->windowed_height, GLFW_DONT_CARE);
        ctx->is_fullscreen = 0;
    } else {
        glfwGetWindowPos(ctx->window, &ctx->windowed_x, &ctx->windowed_y);
        glfwGetWindowSize(ctx->window, &ctx->windowed_width,
                          &ctx->windowed_height);
        GLFWmonitor *monitor = get_current_monitor(ctx->window);
        if (monitor) {
            const GLFWvidmode *mode = glfwGetVideoMode(monitor);
            glfwSetWindowMonitor(ctx->window, monitor, 0, 0, mode->width,
                                 mode->height, mode->refreshRate);
            ctx->is_fullscreen = 1;
        }
    }
}

static void create_contexts(struct gl_ctx ctx[], int num_ctx, double pdf_width,
                            double pdf_height) {
    int base = (int)pdf_width / num_ctx;
    GLFWwindow *share = NULL;
    for (int i = 0; i < num_ctx; i++) {
        int width = (i == num_ctx - 1) ? (int)pdf_width - base * i : base;
        char title[32];
        snprintf(title, sizeof(title), "Context %d", i);
        ctx[i].window =
            glfwCreateWindow(width, (int)pdf_height, title, NULL, share);
        expect(ctx[i].window);
        share = ctx[i].window;
    }
}

static void update_scale(struct prog_state *state) {
    double new_scale = compute_scale(state->ctx, state->num_ctx,
                                     state->pdf_width, state->pdf_height);
    if (fabs(new_scale - state->current_scale) > 0.01) {
        state->current_scale = new_scale;
        fprintf(stderr, "Window resized, new scale: %.2f\n", new_scale);
        free_all_cache_entries(state->cache_entries, state->num_pages);
        state->cache_complete = 0;
        state->cache_entries[state->current_page] =
            create_cache_entry(state->current_page, state);
        state->needs_redraw = 1;
    }
}

static void key_callback(GLFWwindow *window, int key, int scancode, int action,
                         int mods) {
    (void)scancode;

    if (action != GLFW_PRESS)
        return;

    struct prog_state *state = glfwGetWindowUserPointer(window);
    if (key == GLFW_KEY_Q && (mods & GLFW_MOD_SHIFT)) {
        for (int i = 0; i < state->num_ctx; i++)
            glfwSetWindowShouldClose(state->ctx[i].window, GLFW_TRUE);
        return;
    }

    if (key == GLFW_KEY_F && (mods & GLFW_MOD_SHIFT)) {
        for (int i = 0; i < state->num_ctx; i++) {
            if (window == state->ctx[i].window) {
                toggle_fullscreen(&state->ctx[i]);
                break;
            }
        }
        update_scale(state);
        return;
    }

    int new_page = state->current_page;
    if ((key == GLFW_KEY_LEFT || key == GLFW_KEY_UP ||
         key == GLFW_KEY_PAGE_UP) &&
        state->current_page > 0)
        new_page = state->current_page - 1;
    else if ((key == GLFW_KEY_RIGHT || key == GLFW_KEY_DOWN ||
              key == GLFW_KEY_PAGE_DOWN) &&
             state->current_page < state->num_pages - 1) {
        new_page = state->current_page + 1;
    }

    if (new_page != state->current_page) {
        if (!state->cache_entries[new_page]) {
            fprintf(
                stderr,
                "Warning: Page %d not cached; performing blocking render.\n",
                new_page);
            state->cache_entries[new_page] =
                create_cache_entry(new_page, state);
        }
        state->current_page = new_page;
        state->needs_redraw = 1;
    }
}

static void framebuffer_size_callback(GLFWwindow *window, int width,
                                      int height) {
    (void)width;
    (void)height;
    update_scale(glfwGetWindowUserPointer(window));
}

void update_window_textures(struct prog_state *state) {
    struct render_cache_entry *entry =
        state->cache_entries[state->current_page];
    expect(entry);
    for (int i = 0; i < state->num_ctx; i++) {
        present_texture(state->ctx[i].window, entry->textures[i],
                        entry->widths[i], entry->texture_height);
    }
    state->needs_redraw = 0;
}

static void window_refresh_callback(GLFWwindow *window) {
    struct prog_state *state = glfwGetWindowUserPointer(window);
    update_window_textures(state);
}

static void handle_glfw_events(struct prog_state *state) {
    for (int i = 0; i < state->num_ctx; i++) {
        glfwSetKeyCallback(state->ctx[i].window, key_callback);
        glfwSetFramebufferSizeCallback(state->ctx[i].window,
                                       framebuffer_size_callback);
        glfwSetWindowRefreshCallback(state->ctx[i].window,
                                     window_refresh_callback);
        glfwSetWindowUserPointer(state->ctx[i].window, state);
    }

    glfwMakeContextCurrent(state->ctx[0].window);

    /* Render the first page (blocking render); others will be cached. */
    state->cache_entries[state->current_page] =
        create_cache_entry(state->current_page, state);
    if (state->cache_entries[state->current_page]) {
        update_window_textures(state);
    }

    while (!glfwWindowShouldClose(state->ctx[0].window)) {
        if (state->cache_complete)
            glfwWaitEvents();
        else {
            glfwPollEvents(); // If a key has been pressed, that takes priority
            cache_one_slide(state->cache_entries, state->num_pages, state);
        }
        if (state->needs_redraw && state->cache_entries[state->current_page]) {
            update_window_textures(state);
        }
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

    expect(glfwInit());
    struct prog_state ps = {0};
    if (init_prog_state(&ps, pdf_file) < 0)
        return EXIT_FAILURE;
    ps.num_ctx = NUM_CONTEXTS;
    ps.cache_entries =
        calloc(ps.num_pages, sizeof(struct render_cache_entry *));
    expect(ps.cache_entries);
    ps.ctx = calloc(ps.num_ctx, sizeof(struct gl_ctx));
    expect(ps.ctx);
    create_contexts(ps.ctx, ps.num_ctx, ps.pdf_width, ps.pdf_height);

    ps.current_scale =
        compute_scale(ps.ctx, ps.num_ctx, ps.pdf_width, ps.pdf_height);
    ps.needs_redraw = 1;

    handle_glfw_events(&ps);

    fz_drop_document(ps.fz_ctx, ps.document);
    fz_drop_context(ps.fz_ctx);

    for (int i = 0; i < ps.num_ctx; i++) {
        if (ps.ctx[i].window)
            glfwDestroyWindow(ps.ctx[i].window);
    }
    free(ps.ctx);
    glfwTerminate();
}
