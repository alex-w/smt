/* Stellarium Web Engine - Copyright (c) 2018 - Noctua Software Ltd
 *
 * This program is licensed under the terms of the GNU AGPL v3, or
 * alternatively under a commercial licence.
 *
 * The terms of the AGPL v3 license can be found in the main directory of this
 * repository.
 */

#include "swe.h"

#include "line_mesh.h"
#include "shader_cache.h"
#include "utils/gl.h"

#ifdef GLES2
#   define NANOVG_GLES2_IMPLEMENTATION
#else
#   define NANOVG_GL2_IMPLEMENTATION
#endif
#include "nanovg.h"
#include "nanovg_gl.h"

#include <float.h>

#define GRID_CACHE_SIZE (2 * (1 << 20))

// Fix GL_PROGRAM_POINT_SIZE support on Mac.
#ifdef __APPLE__
#   define GL_PROGRAM_POINT_SIZE GL_PROGRAM_POINT_SIZE_EXT
#endif

enum {
    FONT_REGULAR = 0,
    FONT_BOLD    = 1,
};

// All the shader attribute locations.
enum {
    ATTR_POS,
    ATTR_MPOS,
    ATTR_TEX_POS,
    ATTR_NORMAL,
    ATTR_TANGENT,
    ATTR_COLOR,
    ATTR_SKY_POS,
    ATTR_LUMINANCE,
    ATTR_SIZE,
};

static const char *ATTR_NAMES[] = {
    [ATTR_POS]          = "a_pos",
    [ATTR_MPOS]         = "a_mpos",
    [ATTR_TEX_POS]      = "a_tex_pos",
    [ATTR_NORMAL]       = "a_normal",
    [ATTR_TANGENT]      = "a_tangent",
    [ATTR_COLOR]        = "a_color",
    [ATTR_SKY_POS]      = "a_sky_pos",
    [ATTR_LUMINANCE]    = "a_luminance",
    [ATTR_SIZE]         = "a_size",
    NULL,
};

// We keep all the text textures in a cache so that we don't have to recreate
// them each time.
typedef struct tex_cache tex_cache_t;
struct tex_cache {
    tex_cache_t *next, *prev;
    double      size;
    char        *text;
    int         effects;
    bool        in_use;
    int         xoff;
    int         yoff;
    texture_t   *tex;
};

enum {
    ITEM_LINES = 1,
    ITEM_MESH,
    ITEM_POINTS,
    ITEM_TEXTURE,
    ITEM_ATMOSPHERE,
    ITEM_FOG,
    ITEM_PLANET,
    ITEM_VG_ELLIPSE,
    ITEM_VG_RECT,
    ITEM_VG_LINE,
    ITEM_TEXT,
    ITEM_QUAD_WIREFRAME,
    ITEM_LINES_GLOW,
    ITEM_GLTF,
};

typedef struct item item_t;
struct item
{
    int type;

    float       color[4];
    gl_buf_t    buf;
    gl_buf_t    indices;
    texture_t   *tex;
    int         flags;
    float       depth_range[2];

    union {
        struct {
            float width;
            float glow;
            float dash_length;
            float dash_ratio;
            float fade_dist_min;
            float fade_dist_max;
        } lines;

        struct {
            float halo;
        } points;

        struct {
            float contrast;
            texture_t *normalmap;
            texture_t *shadow_color_tex;
            float mv[16];
            float sun[4]; // pos + radius.
            float light_emit[3];
            int   shadow_spheres_nb;
            float shadow_spheres[4][4]; // (pos + radius) * 4
            int   material;
            float tex_transf[9];
            float normal_tex_transf[9];
        } planet;

        struct {
            float pos[2];
            float pos2[2];
            float size[2];
            float angle;
            float dashes;
            float stroke_width;
        } vg;

        struct {
            float p[12];    // Color computation coefs.
            float sun[3];   // Sun position.
        } atm;

        struct {
            char text[128];
            float pos[2];
            float size;
            float angle;
            int   align;
            int   effects;
        } text;

        struct {
            int mode;
            float stroke_width;
            // Projection setttings.  Should be set globally probably.
            int proj;
            float proj_scaling[2];
            bool use_stencil;
        } mesh;

        struct {
            const char *model;
            double model_mat[4][4];
            double view_mat[4][4];
            double proj_mat[4][4];
            double light_dir[3];
            json_value *args;
        } gltf;
    };

    item_t *next, *prev;
};

static const gl_buf_info_t INDICES_BUF = {
    .size = 2,
    .attrs = {
        {GL_UNSIGNED_SHORT, 1, false, 0},
    },
};

static const gl_buf_info_t MESH_BUF = {
    .size = 20,
    .attrs = {
        [ATTR_POS]      = {GL_FLOAT, 4, false, 0},
        [ATTR_COLOR]    = {GL_UNSIGNED_BYTE, 4, true, 16},
    },
};

static const gl_buf_info_t LINES_BUF = {
    .size = 28,
    .attrs = {
        [ATTR_POS]      = {GL_FLOAT, 4, false, 0},
        [ATTR_TEX_POS]  = {GL_FLOAT, 2, false, 16},
        [ATTR_COLOR]    = {GL_UNSIGNED_BYTE, 4, true, 24},
    },
};

static const gl_buf_info_t LINES_GLOW_BUF = {
    .size = 20,
    .attrs = {
        [ATTR_POS]      = {GL_FLOAT, 3, false, 0},
        [ATTR_TEX_POS]  = {GL_FLOAT, 2, false, 12},
    },
};

static const gl_buf_info_t POINTS_BUF = {
    .size = 16,
    .attrs = {
        [ATTR_POS]      = {GL_FLOAT, 2, false, 0},
        [ATTR_SIZE]     = {GL_FLOAT, 1, false, 8},
        [ATTR_COLOR]    = {GL_UNSIGNED_BYTE, 4, true, 12},
    },
};

static const gl_buf_info_t TEXTURE_BUF = {
    .size = 24,
    .attrs = {
        [ATTR_POS]      = {GL_FLOAT, 4, false, 0},
        [ATTR_TEX_POS]  = {GL_FLOAT, 2, false, 16},
    },
};

static const gl_buf_info_t PLANET_BUF = {
    .size = 68,
    .attrs = {
        [ATTR_POS]      = {GL_FLOAT, 4, false, 0},
        [ATTR_MPOS]     = {GL_FLOAT, 4, false, 16},
        [ATTR_TEX_POS]  = {GL_FLOAT, 2, false, 32},
        [ATTR_COLOR]    = {GL_UNSIGNED_BYTE, 4, true, 40},
        [ATTR_NORMAL]   = {GL_FLOAT, 3, false, 44},
        [ATTR_TANGENT]  = {GL_FLOAT, 3, false, 56},
    },
};

static const gl_buf_info_t ATMOSPHERE_BUF = {
    .size = 32,
    .attrs = {
        [ATTR_POS]       = {GL_FLOAT, 4, false, 0},
        [ATTR_SKY_POS]   = {GL_FLOAT, 3, false, 16},
        [ATTR_LUMINANCE] = {GL_FLOAT, 1, false, 28},
    },
};

static const gl_buf_info_t FOG_BUF = {
    .size = 28,
    .attrs = {
        [ATTR_POS]       = {GL_FLOAT, 4, false, 0},
        [ATTR_SKY_POS]   = {GL_FLOAT, 3, false, 16},
    },
};

typedef struct renderer_gl {
    renderer_t  rend;

    int     fb_size[2];
    double  scale;
    bool    cull_flipped;
    double  depth_range[2];

    texture_t   *white_tex;
    tex_cache_t *tex_cache;
    NVGcontext *vg;

    // Nanovg fonts references for regular and bold.
    struct {
        int   id;
        float scale;
        bool  is_default_font; // Set only for the original default fonts.
    } fonts[2];

    item_t  *items;
    cache_t *grid_cache;

} renderer_gl_t;

// Weak linking, so that we can put the implementation in a module.
__attribute__((weak))
int gltf_render(const char *url,
                const double model_mat[4][4],
                const double view_mat[4][4],
                const double proj_mat[4][4],
                const double light_dir[3],
                json_value *args)
{
    return 0;
}

static void init_shader(gl_shader_t *shader)
{
    // Set some common uniforms.
    GL(glUseProgram(shader->prog));
    gl_update_uniform(shader, "u_tex", 0);
    gl_update_uniform(shader, "u_normal_tex", 1);
    gl_update_uniform(shader, "u_shadow_color_tex", 2);
}

static bool color_is_white(const float c[4])
{
    return c[0] == 1.0f && c[1] == 1.0f && c[2] == 1.0f && c[3] == 1.0f;
}

static void window_to_ndc(renderer_gl_t *rend,
                          const double win[2], double ndc[2])
{
    ndc[0] = (win[0] * rend->scale / rend->fb_size[0]) * 2 - 1;
    ndc[1] = 1 - (win[1] * rend->scale / rend->fb_size[1]) * 2;
}

static void prepare(renderer_t *rend_, double win_w, double win_h,
                    double scale, bool cull_flipped)
{
    renderer_gl_t *rend = (void*)rend_;
    tex_cache_t *ctex;

    rend->fb_size[0] = win_w * scale;
    rend->fb_size[1] = win_h * scale;
    rend->scale = scale;
    rend->cull_flipped = cull_flipped;

    DL_FOREACH(rend->tex_cache, ctex)
        ctex->in_use = false;
}

/*
 * Function: get_item
 * Try to get a render item we can batch with.
 *
 * Parameters:
 *   type           - The type of item.
 *   buf_size       - The free vertex buffer size requiered.
 *   indices_size   - The free indice size required.
 */
static item_t *get_item(renderer_gl_t *rend, int type,
                        int buf_size,
                        int indices_size,
                        texture_t *tex)
{
    item_t *item;
    item = rend->items ? rend->items->prev : NULL;

    while (item) {
        if (item->type == type &&
            item->buf.capacity > item->buf.nb + buf_size &&
            (indices_size == 0 ||
                item->indices.capacity > item->indices.nb + indices_size) &&
            item->tex == tex)
        {
            return item;
        }
        // Keep searching only if we allow reordering.
        if (!(item->flags & PAINTER_ALLOW_REORDER)) break;
        item = item->prev;
    }
    return NULL;
}

static void points_2d(renderer_t *rend_,
                      const painter_t *painter,
                      int n,
                      const point_t *points)
{
    renderer_gl_t *rend = (void*)rend_;
    item_t *item;
    int i;
    const int MAX_POINTS = 4096;
    point_t p;

    if (n > MAX_POINTS) {
        LOG_E("Try to render more than %d points: %d", MAX_POINTS, n);
        n = MAX_POINTS;
    }

    item = get_item(rend, ITEM_POINTS, n, 0, NULL);
    if (item && item->points.halo != painter->points_halo)
        item = NULL;
    if (!item) {
        item = calloc(1, sizeof(*item));
        item->type = ITEM_POINTS;
        gl_buf_alloc(&item->buf, &POINTS_BUF, MAX_POINTS);
        vec4_to_float(painter->color, item->color);
        item->points.halo = painter->points_halo;
        DL_APPEND(rend->items, item);
    }

    for (i = 0; i < n; i++) {
        p = points[i];
        window_to_ndc(rend, p.pos, p.pos);

        gl_buf_2f(&item->buf, -1, ATTR_POS, VEC2_SPLIT(p.pos));
        gl_buf_1f(&item->buf, -1, ATTR_SIZE, p.size * rend->scale);
        gl_buf_4i(&item->buf, -1, ATTR_COLOR, VEC4_SPLIT(p.color));
        gl_buf_next(&item->buf);

        // Add the point int the global list of rendered points.
        // XXX: could be done in the painter.
        if (p.obj) {
            p.pos[0] = (+p.pos[0] + 1) / 2 * core->win_size[0];
            p.pos[1] = (-p.pos[1] + 1) / 2 * core->win_size[1];
            areas_add_circle(core->areas, p.pos, p.size, p.obj);
        }
    }
}

/*
 * Function: get_grid
 * Compute an uv_map grid, and cache it if possible.
 */
static const double (*get_grid(renderer_gl_t *rend,
                               const uv_map_t *map, int split,
                               bool *should_delete))[4]
{
    int n = split + 1;
    double (*grid)[4];
    struct {
        int order;
        int pix;
        int split;
        int swapped;
    } key = { map->order, map->pix, split, map->swapped };
    _Static_assert(sizeof(key) == 16, "");
    bool can_cache = map->type == UV_MAP_HEALPIX && map->at_infinity;

    *should_delete = !can_cache;
    if (can_cache) {
        if (!rend->grid_cache)
            rend->grid_cache = cache_create(GRID_CACHE_SIZE);
        grid = cache_get(rend->grid_cache, &key, sizeof(key));
        if (grid)
            return grid;
    }

    grid = malloc(n * n * sizeof(*grid));
    uv_map_grid(map, split, grid, NULL);

    if (can_cache) {
        cache_add(rend->grid_cache, &key, sizeof(key),
                  grid, sizeof(*grid) * n * n, NULL);
    }

    return grid;
}

static void compute_tangent(const double uv[2], const uv_map_t *map,
                            double out[3])
{
    // XXX: this is what the algo should look like, except the normal map
    // texture we use (for the Moon) doesn't follow the healpix projection.
    /*
    double uv1[2], uv2[2], p1[4], p2[4], tangent[3];
    const double delta = 0.1;
    vec2_copy(uv, uv1);
    vec2_copy(uv, uv2);
    uv2[0] += delta;
    project(tex_proj, PROJ_BACKWARD, 4, uv1, p1);
    project(tex_proj, PROJ_BACKWARD, 4, uv2, p2);
    vec3_sub(p2, p1, tangent);
    vec3_normalize(tangent, out);
    */

    double p[4] = {0}, n[3];
    uv_map(map, uv, p, n);
    vec3_cross(VEC(0, 0, 1), n, out);
}

static void quad_planet(
                 renderer_t          *rend_,
                 const painter_t     *painter,
                 int                 frame,
                 int                 grid_size,
                 const uv_map_t      *map)
{
    renderer_gl_t *rend = (void*)rend_;
    item_t *item;
    int n, i, j, k;
    double p[4], mpos[4], normal[4] = {0}, tangent[4] = {0}, z, mv[4][4];

    // Positions of the triangles in the quads.
    const int INDICES[6][2] = { {0, 0}, {0, 1}, {1, 0},
                                {1, 1}, {1, 0}, {0, 1} };
    n = grid_size + 1;

    item = calloc(1, sizeof(*item));
    item->type = ITEM_PLANET;
    gl_buf_alloc(&item->buf, &PLANET_BUF, n * n * 4);
    gl_buf_alloc(&item->indices, &INDICES_BUF, n * n * 6);
    vec4_to_float(painter->color, item->color);
    item->flags = painter->flags;
    item->planet.shadow_color_tex = painter->planet.shadow_color_tex;
    item->planet.contrast = painter->contrast;
    item->planet.shadow_spheres_nb = painter->planet.shadow_spheres_nb;
    for (i = 0; i < painter->planet.shadow_spheres_nb; i++) {
        vec4_to_float(painter->planet.shadow_spheres[i],
                      item->planet.shadow_spheres[i]);
    }
    vec4_to_float(*painter->planet.sun, item->planet.sun);
    if (painter->planet.light_emit)
        vec3_to_float(*painter->planet.light_emit, item->planet.light_emit);

    // Compute modelview matrix.
    mat4_set_identity(mv);
    if (frame == FRAME_OBSERVED)
        mat3_to_mat4(painter->obs->ro2v, mv);
    if (frame == FRAME_ICRF)
        mat3_to_mat4(painter->obs->ri2v, mv);
    mat4_to_float(mv, item->planet.mv);

    // Set material
    if (painter->planet.light_emit)
        item->planet.material = 1; // Generic
    else
        item->planet.material = 0; // Oren Nayar
    if (painter->flags & PAINTER_RING_SHADER)
        item->planet.material = 2; // Ring

    // Set textures.
    item->tex = painter->textures[PAINTER_TEX_COLOR].tex ?: rend->white_tex;
    mat3_to_float(painter->textures[PAINTER_TEX_COLOR].mat,
                  item->planet.tex_transf);
    item->tex->ref++;
    item->planet.normalmap = painter->textures[PAINTER_TEX_NORMAL].tex;
    mat3_to_float(painter->textures[PAINTER_TEX_NORMAL].mat,
                  item->planet.normal_tex_transf);
    if (item->planet.normalmap) item->planet.normalmap->ref++;

    // Only support POT textures for planets.
    assert(item->tex->w == item->tex->tex_w &&
           item->tex->h == item->tex->tex_h);

    for (i = 0; i < n; i++)
    for (j = 0; j < n; j++) {
        vec3_set(p, (double)j / grid_size, (double)i / grid_size, 1.0);
        gl_buf_2f(&item->buf, -1, ATTR_TEX_POS, p[0], p[1]);
        if (item->planet.normalmap) {
            compute_tangent(p, map, tangent);
            gl_buf_3f(&item->buf, -1, ATTR_TANGENT, VEC3_SPLIT(tangent));
        }

        vec3_set(p, (double)j / grid_size, (double)i / grid_size, 1.0);
        uv_map(map, p, p, normal);
        assert(p[3] == 1.0); // Planet can never be at infinity.

        gl_buf_3f(&item->buf, -1, ATTR_NORMAL, VEC3_SPLIT(normal));

        // Model position (without scaling applied).
        vec4_copy(p, mpos);
        assert(map->transf);
        vec3_sub(mpos, (*map->transf)[3], mpos);
        vec3_mul(1.0 / painter->planet.scale, mpos, mpos);
        vec3_add(mpos, (*map->transf)[3], mpos);
        gl_buf_4f(&item->buf, -1, ATTR_MPOS, VEC4_SPLIT(mpos));

        // Rendering position (with scaling applied).
        convert_framev4(painter->obs, frame, FRAME_VIEW, p, p);
        z = p[2];
        project(painter->proj, 0, p, p);
        if (painter->depth_range) {
            vec2_to_float(*painter->depth_range, item->depth_range);
            p[2] = -z;
        }
        gl_buf_4f(&item->buf, -1, ATTR_POS, VEC4_SPLIT(p));
        gl_buf_4i(&item->buf, -1, ATTR_COLOR, 255, 255, 255, 255);
        gl_buf_next(&item->buf);
    }

    for (i = 0; i < grid_size; i++)
    for (j = 0; j < grid_size; j++) {
        for (k = 0; k < 6; k++) {
            gl_buf_1i(&item->indices, -1, 0,
                      (INDICES[k][1] + i) * n + (INDICES[k][0] + j));
            gl_buf_next(&item->indices);
        }
    }
    DL_APPEND(rend->items, item);
}

static void quad(renderer_t          *rend_,
                 const painter_t     *painter,
                 int                 frame,
                 int                 grid_size,
                 const uv_map_t      *map)
{
    renderer_gl_t *rend = (void*)rend_;
    item_t *item;
    int n, i, j, k, ofs;
    const int INDICES[6][2] = {
        {0, 0}, {0, 1}, {1, 0}, {1, 1}, {1, 0}, {0, 1} };
    double p[4], tex_pos[2], ndc_p[4];
    float lum;
    const double (*grid)[4] = NULL;
    bool should_delete_grid;
    texture_t *tex = painter->textures[PAINTER_TEX_COLOR].tex;

    // Special case for planet shader.
    if (painter->flags & (PAINTER_PLANET_SHADER | PAINTER_RING_SHADER))
        return quad_planet(rend_, painter, frame, grid_size, map);

    if (!tex) tex = rend->white_tex;
    n = grid_size + 1;

    if (painter->flags & PAINTER_ATMOSPHERE_SHADER) {
        item = get_item(rend, ITEM_ATMOSPHERE,
                        n * n, grid_size * grid_size * 6, tex);
        if (item && (
                memcmp(item->atm.p, painter->atm.p, sizeof(item->atm.p)) ||
                memcmp(item->atm.sun, painter->atm.sun, sizeof(item->atm.sun))))
            item = NULL;
        if (!item) {
            item = calloc(1, sizeof(*item));
            item->type = ITEM_ATMOSPHERE;
            gl_buf_alloc(&item->buf, &ATMOSPHERE_BUF, 256);
            gl_buf_alloc(&item->indices, &INDICES_BUF, 256 * 6);
            memcpy(item->atm.p, painter->atm.p, sizeof(item->atm.p));
            memcpy(item->atm.sun, painter->atm.sun, sizeof(item->atm.sun));
        }
    } else if (painter->flags & PAINTER_FOG_SHADER) {
        item = get_item(rend, ITEM_FOG, n * n, grid_size * grid_size * 6, tex);
        if (!item) {
            item = calloc(1, sizeof(*item));
            item->type = ITEM_FOG;
            gl_buf_alloc(&item->buf, &FOG_BUF, 256);
            gl_buf_alloc(&item->indices, &INDICES_BUF, 256 * 6);
        }
    } else {
        item = calloc(1, sizeof(*item));
        item->type = ITEM_TEXTURE;
        gl_buf_alloc(&item->buf, &TEXTURE_BUF, n * n);
        gl_buf_alloc(&item->indices, &INDICES_BUF, n * n * 6);
    }

    ofs = item->buf.nb;
    item->tex = tex;
    item->tex->ref++;
    vec4_to_float(painter->color, item->color);
    item->flags = painter->flags;

    grid = get_grid(rend, map, grid_size, &should_delete_grid);
    for (i = 0; i < n; i++)
    for (j = 0; j < n; j++) {
        vec3_set(p, (double)j / grid_size, (double)i / grid_size, 1.0);
        mat3_mul_vec3(painter->textures[PAINTER_TEX_COLOR].mat, p, p);
        tex_pos[0] = p[0] * tex->w / tex->tex_w;
        tex_pos[1] = p[1] * tex->h / tex->tex_h;
        gl_buf_2f(&item->buf, -1, ATTR_TEX_POS, tex_pos[0], tex_pos[1]);

        vec4_set(p, VEC4_SPLIT(grid[i * n + j]));
        convert_framev4(painter->obs, frame, FRAME_VIEW, p, ndc_p);
        project(painter->proj, 0, ndc_p, ndc_p);
        gl_buf_4f(&item->buf, -1, ATTR_POS, VEC4_SPLIT(ndc_p));
        // For atmosphere shader, in the first pass we do not compute the
        // luminance yet, only if the point is visible.
        if (painter->flags & PAINTER_ATMOSPHERE_SHADER) {
            gl_buf_3f(&item->buf, -1, ATTR_SKY_POS, VEC3_SPLIT(p));
            lum = painter->atm.compute_lum(painter->atm.user,
                    (float[3]){p[0], p[1], p[2]});
            gl_buf_1f(&item->buf, -1, ATTR_LUMINANCE, lum);
        }
        if (painter->flags & PAINTER_FOG_SHADER) {
            gl_buf_3f(&item->buf, -1, ATTR_SKY_POS, VEC3_SPLIT(p));
        }
        gl_buf_next(&item->buf);
    }
    if (should_delete_grid) free(grid);

    // Set the index buffer.
    for (i = 0; i < grid_size; i++)
    for (j = 0; j < grid_size; j++) {
        for (k = 0; k < 6; k++) {
            gl_buf_1i(&item->indices, -1, 0,
                      ofs + (INDICES[k][1] + i) * n + (INDICES[k][0] + j));
            gl_buf_next(&item->indices);
        }
    }
    DL_APPEND(rend->items, item);
}

static void quad_wireframe(renderer_t          *rend_,
                           const painter_t     *painter,
                           int                 frame,
                           int                 grid_size,
                           const uv_map_t      *map)
{
    renderer_gl_t *rend = (void*)rend_;
    int n, i, j;
    item_t *item;
    n = grid_size + 1;
    double p[4], ndc_p[4];
    const double (*grid)[4] = NULL;
    bool should_delete_grid;

    item = calloc(1, sizeof(*item));
    item->type = ITEM_QUAD_WIREFRAME;
    gl_buf_alloc(&item->buf, &TEXTURE_BUF, n * n);
    gl_buf_alloc(&item->indices, &INDICES_BUF, grid_size * n * 4);
    vec4_to_float(VEC(1, 0, 0, 0.25), item->color);

    // Generate grid position.
    grid = get_grid(rend, map, grid_size, &should_delete_grid);
    for (i = 0; i < n; i++)
    for (j = 0; j < n; j++) {
        gl_buf_2f(&item->buf, -1, ATTR_TEX_POS, 0.5, 0.5);
        vec4_set(p, VEC4_SPLIT(grid[i * n + j]));
        convert_framev4(painter->obs, frame, FRAME_VIEW, p, ndc_p);
        project(painter->proj, 0, ndc_p, ndc_p);
        gl_buf_4f(&item->buf, -1, ATTR_POS, VEC4_SPLIT(ndc_p));
        gl_buf_next(&item->buf);
    }
    if (should_delete_grid) free(grid);

    /* Set the index buffer.
     * We render a set of horizontal and vertical lines.  */
    for (i = 0; i < n; i++)
    for (j = 0; j < grid_size; j++) {
        // Vertical.
        gl_buf_1i(&item->indices, -1, 0, (j + 0) * n + i);
        gl_buf_next(&item->indices);
        gl_buf_1i(&item->indices, -1, 0, (j + 1) * n + i);
        gl_buf_next(&item->indices);
        // Horizontal.
        gl_buf_1i(&item->indices, -1, 0, i * n + j + 0);
        gl_buf_next(&item->indices);
        gl_buf_1i(&item->indices, -1, 0, i * n + j + 1);
        gl_buf_next(&item->indices);
    }
    DL_APPEND(rend->items, item);
}

static void texture2(renderer_gl_t *rend, texture_t *tex,
                     double uv[4][2], double pos[4][2],
                     const double color_[4], int flags, bool swap_indices)
{
    int i, ofs;
    item_t *item;
    const int16_t INDICES[6] = {0, 1, 2, 3, 2, 1 };
    float color[4];

    vec4_to_float(color_, color);
    item = get_item(rend, ITEM_TEXTURE, 4, 6, tex);
    if (item && memcmp(item->color, color, sizeof(color))) item = NULL;

    if (!item) {
        item = calloc(1, sizeof(*item));
        item->type = ITEM_TEXTURE;
        item->flags = flags;
        gl_buf_alloc(&item->buf, &TEXTURE_BUF, 64 * 4);
        gl_buf_alloc(&item->indices, &INDICES_BUF, 64 * 6);
        item->tex = tex;
        item->tex->ref++;
        memcpy(item->color, color, sizeof(color));
        DL_APPEND(rend->items, item);
    }

    ofs = item->buf.nb;

    for (i = 0; i < 4; i++) {
        gl_buf_4f(&item->buf, -1, ATTR_POS, pos[i][0], pos[i][1], 0.0, 1.0);
        gl_buf_2f(&item->buf, -1, ATTR_TEX_POS, uv[i][0], uv[i][1]);
        gl_buf_next(&item->buf);
    }
    for (i = 0; i < 6; i++) {
        if (swap_indices)
            gl_buf_1i(&item->indices, -1, 0, ofs + INDICES[5 - i]);
        else
            gl_buf_1i(&item->indices, -1, 0, ofs + INDICES[i]);
        gl_buf_next(&item->indices);
    }
}

static void texture(renderer_t *rend_,
                    const texture_t *tex,
                    double uv[4][2],
                    const double pos[2],
                    double size,
                    const double color[4],
                    double angle)
{
    renderer_gl_t *rend = (void*)rend_;
    int i;
    double verts[4][2], w, h;
    w = size;
    h = size * tex->h / tex->w;
    for (i = 0; i < 4; i++) {
        verts[i][0] = (i % 2 - 0.5) * w;
        verts[i][1] = (0.5 - i / 2) * h;
        if (angle != 0.0) vec2_rotate(-angle, verts[i], verts[i]);
        verts[i][0] = pos[0] + verts[i][0];
        verts[i][1] = pos[1] + verts[i][1];
        window_to_ndc(rend, verts[i], verts[i]);
    }
    texture2(rend, tex, uv, verts, color, 0, false);
}

// Render text using a system bakend generated texture.
static void text_using_texture(renderer_gl_t *rend,
                               const painter_t *painter,
                               const char *text, const double pos[2],
                               int align, int effects, double size,
                               const double color[4], double angle,
                               double out_bounds[4])
{
    double uv[4][2], verts[4][2];
    double s[2], ofs[2] = {0, 0}, bounds[4];
    const double scale = rend->scale;
    uint8_t *img;
    int i, w, h, xoff, yoff, flags;
    tex_cache_t *ctex;
    texture_t *tex;

    DL_FOREACH(rend->tex_cache, ctex) {
        if (ctex->size == size && ctex->effects == effects &&
                strcmp(ctex->text, text) == 0) break;
    }

    if (!ctex) {
        img = (void*)sys_render_text(text, size * scale, effects, &w, &h,
                                     &xoff, &yoff);
        ctex = calloc(1, sizeof(*ctex));
        ctex->size = size;
        ctex->effects = effects;
        ctex->xoff = xoff;
        ctex->yoff = yoff;
        ctex->text = strdup(text);
        ctex->tex = texture_from_data(img, w, h, 1, 0, 0, w, h, 0);
        free(img);
        DL_APPEND(rend->tex_cache, ctex);
    }

    ctex->in_use = true;

    // Compute bounds taking alignment into account.
    s[0] = ctex->tex->w / scale;
    s[1] = ctex->tex->h / scale;
    if (align & ALIGN_LEFT)     ofs[0] = +s[0] / 2;
    if (align & ALIGN_RIGHT)    ofs[0] = -s[0] / 2;
    if (align & ALIGN_TOP)      ofs[1] = +s[1] / 2;
    if (align & ALIGN_BOTTOM)   ofs[1] = -s[1] / 2;
    bounds[0] = pos[0] - s[0] / 2 + ofs[0] + ctex->xoff / scale;
    bounds[1] = pos[1] - s[1] / 2 + ofs[1] + ctex->yoff / scale;

    // Round the position to the nearest pixel.  We add a small delta to
    // fix a bug when we are exactly in between two pixels, which can happen
    // for example with the label of a centered object.
    if (!angle) bounds[0] = round(bounds[0] * scale + 0.000001) / scale;
    if (!angle) bounds[1] = round(bounds[1] * scale + 0.000001) / scale;

    bounds[2] = bounds[0] + s[0];
    bounds[3] = bounds[1] + s[1];

    if (out_bounds) {
        memcpy(out_bounds, bounds, sizeof(bounds));
        return;
    }
    tex = ctex->tex;

    /*
     * Render the texture, being careful to do the rotation centered on
     * the anchor point.
     */
    for (i = 0; i < 4; i++) {
        uv[i][0] = ((i % 2) * tex->w) / (double)tex->tex_w;
        uv[i][1] = ((i / 2) * tex->h) / (double)tex->tex_h;
        verts[i][0] = (i % 2 - 0.5) * tex->w / scale;
        verts[i][1] = (0.5 - i / 2) * tex->h / scale;
        verts[i][0] += ofs[0];
        verts[i][1] += ofs[1];
        vec2_rotate(angle, verts[i], verts[i]);
        verts[i][0] -= ofs[0];
        verts[i][1] -= ofs[1];
        verts[i][0] += (bounds[0] + bounds[2]) / 2;
        verts[i][1] += (bounds[1] + bounds[3]) / 2;
        window_to_ndc(rend, verts[i], verts[i]);
    }

    flags = painter->flags;
    if (effects & TEXT_BLEND_ADD) flags |= PAINTER_ADD;
    texture2(rend, tex, uv, verts, color, flags, rend->cull_flipped);
}

// Render text using nanovg.
static void text_using_nanovg(renderer_gl_t *rend,
                              const painter_t *painter,
                              const char *text,
                              const double pos[2], int align, int effects,
                              double size, const double color[4], double angle,
                              double bounds[4])
{
    item_t *item;
    float fbounds[4];
    int font = (effects & TEXT_BOLD) ? FONT_BOLD : FONT_REGULAR;

    if (strlen(text) >= sizeof(item->text.text)) {
        LOG_W("Text too large: %s", text);
        return;
    }

    if (!bounds) {
        item = calloc(1, sizeof(*item));
        item->type = ITEM_TEXT;
        item->flags = painter->flags;
        vec4_to_float(color, item->color);
        item->color[0] = clamp(item->color[0], 0.0, 1.0);
        item->color[1] = clamp(item->color[1], 0.0, 1.0);
        item->color[2] = clamp(item->color[2], 0.0, 1.0);
        item->color[3] = clamp(item->color[3], 0.0, 1.0);
        vec2_to_float(pos, item->text.pos);
        item->text.size = size;
        item->text.align = align;
        item->text.effects = effects;
        item->text.angle = angle;
        if (effects & TEXT_UPPERCASE || effects & TEXT_SMALL_CAP) {
            // Emulate Small Cap effect by doing regular capitalize
            u8_upper(item->text.text, text, sizeof(item->text.text) - 1);
        } else {
            strcpy(item->text.text, text);
        }
        DL_APPEND(rend->items, item);
    }
    if (bounds) {
        nvgSave(rend->vg);
        nvgFontFaceId(rend->vg, rend->fonts[font].id);
        nvgFontSize(rend->vg, size * rend->fonts[font].scale);
        nvgTextAlign(rend->vg, align);
        nvgTextBounds(rend->vg, pos[0], pos[1], text, NULL, fbounds);
        bounds[0] = fbounds[0];
        bounds[1] = fbounds[1];
        bounds[2] = fbounds[2];
        bounds[3] = fbounds[3];
        nvgRestore(rend->vg);
    }
}

static void text(renderer_t *rend_, const painter_t *painter,
                 const char *text, const double pos[2],
                 int align, int effects, double size, const double color[4],
                 double angle, double bounds[4])
{
    assert(pos);
    renderer_gl_t *rend = (void*)rend_;
    assert(size);

    // Prevent overflow in nvg.
    if (fabs(pos[0]) > 100000 || fabs(pos[1]) > 100000) {
        LOG_W_ONCE("Render text far outside screen: %s, %f %f",
                   text, pos[0], pos[1]);
        if (bounds) {
            bounds[0] = pos[0];
            bounds[1] = pos[1];
        }
        return;
    }

    if (sys_callbacks.render_text) {
        text_using_texture(rend, painter, text, pos, align, effects, size,
                           color, angle, bounds);
    } else {
        text_using_nanovg(rend, painter, text, pos, align, effects, size,
                          color, angle, bounds);
    }

}

static void item_points_render(renderer_gl_t *rend, const item_t *item)
{
    gl_shader_t *shader;
    GLuint  array_buffer;
    double core_size;

    if (item->buf.nb <= 0) {
        LOG_W("Empty point buffer");
        return;
    }

    shader = shader_get("points", NULL, ATTR_NAMES, init_shader);
    GL(glUseProgram(shader->prog));

    GL(glEnable(GL_BLEND));
    GL(glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ZERO, GL_ONE));
    GL(glDisable(GL_DEPTH_TEST));

    GL(glGenBuffers(1, &array_buffer));
    GL(glBindBuffer(GL_ARRAY_BUFFER, array_buffer));
    GL(glBufferData(GL_ARRAY_BUFFER, item->buf.nb * item->buf.info->size,
                    item->buf.data, GL_DYNAMIC_DRAW));

    gl_update_uniform(shader, "u_color", item->color);
    core_size = 1.0 / item->points.halo;
    gl_update_uniform(shader, "u_core_size", core_size);

    gl_buf_enable(&item->buf);
    GL(glDrawArrays(GL_POINTS, 0, item->buf.nb));
    gl_buf_disable(&item->buf);

    GL(glDeleteBuffers(1, &array_buffer));
}

static void draw_buffer(const gl_buf_t *buf, const gl_buf_t *indices,
                        GLuint gl_mode)
{
    GLuint  array_buffer;
    GLuint  index_buffer;

    GL(glGenBuffers(1, &index_buffer));
    GL(glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, index_buffer));
    GL(glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                    indices->nb * indices->info->size,
                    indices->data, GL_DYNAMIC_DRAW));

    GL(glGenBuffers(1, &array_buffer));
    GL(glBindBuffer(GL_ARRAY_BUFFER, array_buffer));
    GL(glBufferData(GL_ARRAY_BUFFER, buf->nb * buf->info->size,
                    buf->data, GL_DYNAMIC_DRAW));

    gl_buf_enable(buf);
    GL(glDrawElements(gl_mode, indices->nb, GL_UNSIGNED_SHORT, 0));
    gl_buf_disable(buf);

    GL(glDeleteBuffers(1, &array_buffer));
    GL(glDeleteBuffers(1, &index_buffer));
}

static void item_lines_render(renderer_gl_t *rend, const item_t *item)
{
    gl_shader_t *shader;
    shader = shader_get("blit", NULL, ATTR_NAMES, init_shader);
    GL(glUseProgram(shader->prog));

    GL(glLineWidth(item->lines.width * rend->scale));

    GL(glActiveTexture(GL_TEXTURE0));
    GL(glBindTexture(GL_TEXTURE_2D, rend->white_tex->id));

    GL(glEnable(GL_BLEND));
    GL(glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                           GL_ZERO, GL_ONE));
    GL(glDisable(GL_DEPTH_TEST));

    draw_buffer(&item->buf, &item->indices, GL_LINES);
}

static void item_mesh_render(renderer_gl_t *rend, const item_t *item)
{
    // XXX: almost the same as item_lines_render.
    gl_shader_t *shader;
    int gl_mode;
    float fbo_size[2] = {rend->fb_size[0] / rend->scale,
                         rend->fb_size[1] / rend->scale};

    gl_mode = item->mesh.mode == 0 ? GL_TRIANGLES :
              item->mesh.mode == 1 ? GL_LINES :
              item->mesh.mode == 2 ? GL_POINTS : 0;

    shader_define_t defines[] = {
        {"PROJ_MOLLWEIDE", item->mesh.proj == PROJ_MOLLWEIDE},
        {}
    };
    shader = shader_get("mesh", defines, ATTR_NAMES, init_shader);
    GL(glUseProgram(shader->prog));

    GL(glLineWidth(item->mesh.stroke_width));

    // For the moment we disable culling for mesh.  We should reintroduce it
    // by making sure we use the proper value depending on the render
    // culling and frame.
    GL(glDisable(GL_CULL_FACE));
    GL(glDisable(GL_DEPTH_TEST));

    GL(glEnable(GL_BLEND));
    GL(glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                           GL_ZERO, GL_ONE));

    // Stencil hack to remove projection deformations artifacts.
    if (item->mesh.use_stencil) {
        GL(glClear(GL_STENCIL_BUFFER_BIT));
        GL(glEnable(GL_STENCIL_TEST));
        GL(glStencilFunc(GL_NOTEQUAL, 1, 0xFF));
        GL(glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE));
    }

    gl_update_uniform(shader, "u_fbo_size", fbo_size);
    gl_update_uniform(shader, "u_proj_scaling", item->mesh.proj_scaling);

    draw_buffer(&item->buf, &item->indices, gl_mode);

    if (item->mesh.use_stencil) {
        GL(glDisable(GL_STENCIL_TEST));
        GL(glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP));
    }
}

// XXX: almost the same as item_mesh_render!
static void item_lines_glow_render(renderer_gl_t *rend, const item_t *item)
{
    gl_shader_t *shader;
    float win_size[2] = {rend->fb_size[0] / rend->scale,
                         rend->fb_size[1] / rend->scale};
    const float depth_range[] = {rend->depth_range[0], rend->depth_range[1]};
    bool use_depth = item->depth_range[0] || item->depth_range[1];

    shader_define_t defines[] = {
        {"DASH", item->lines.dash_length && (item->lines.dash_ratio < 1.0)},
        {"USE_DEPTH", use_depth},
        {"FADE", item->lines.fade_dist_min},
        {}
    };
    shader = shader_get("lines", defines, ATTR_NAMES, init_shader);
    GL(glUseProgram(shader->prog));

    GL(glEnable(GL_BLEND));
    GL(glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                           GL_ZERO, GL_ONE));
    if (use_depth)
        GL(glEnable(GL_DEPTH_TEST));

    gl_update_uniform(shader, "u_line_width", item->lines.width);
    gl_update_uniform(shader, "u_line_glow", item->lines.glow);
    gl_update_uniform(shader, "u_color", item->color);
    gl_update_uniform(shader, "u_win_size", win_size);
    gl_update_uniform(shader, "u_depth_range", depth_range);

    gl_update_uniform(shader, "u_dash_length", item->lines.dash_length);
    gl_update_uniform(shader, "u_dash_ratio", item->lines.dash_ratio);

    if (item->lines.fade_dist_min) {
        gl_update_uniform(shader, "u_fade_dist_min", item->lines.fade_dist_min);
        gl_update_uniform(shader, "u_fade_dist_max", item->lines.fade_dist_max);
    }

    draw_buffer(&item->buf, &item->indices, GL_TRIANGLES);
    GL(glDisable(GL_DEPTH_TEST));
}

static void item_vg_render(renderer_gl_t *rend, const item_t *item)
{
    double a, da;
    nvgBeginFrame(rend->vg, rend->fb_size[0] / rend->scale,
                            rend->fb_size[1] / rend->scale, rend->scale);
    nvgSave(rend->vg);
    nvgTranslate(rend->vg, item->vg.pos[0], item->vg.pos[1]);
    nvgRotate(rend->vg, item->vg.angle);
    nvgBeginPath(rend->vg);

    if (item->type == ITEM_VG_ELLIPSE && !item->vg.dashes)
        nvgEllipse(rend->vg, 0, 0, item->vg.size[0], item->vg.size[1]);

    if (item->type == ITEM_VG_ELLIPSE && item->vg.dashes) {
        da = 2 * M_PI / item->vg.dashes;
        for (a = 0; a < 2 * M_PI; a += da) {
            nvgMoveTo(rend->vg, item->vg.size[0] * cos(a),
                                item->vg.size[1] * sin(a));
            nvgLineTo(rend->vg, item->vg.size[0] * cos(a + da / 2),
                                item->vg.size[1] * sin(a + da / 2));
        }
    }

    if (item->type == ITEM_VG_RECT)
        nvgRect(rend->vg, -item->vg.size[0], -item->vg.size[1],
                2 * item->vg.size[0], 2 * item->vg.size[1]);

    if (item->type == ITEM_VG_LINE) {
        nvgMoveTo(rend->vg, 0, 0);
        nvgLineTo(rend->vg, item->vg.pos2[0] - item->vg.pos[0],
                            item->vg.pos2[1] - item->vg.pos[1]);
    }

    nvgStrokeColor(rend->vg, nvgRGBA(item->color[0] * 255,
                                     item->color[1] * 255,
                                     item->color[2] * 255,
                                     item->color[3] * 255));
    nvgStrokeWidth(rend->vg, item->vg.stroke_width);
    nvgStroke(rend->vg);
    nvgRestore(rend->vg);
    nvgEndFrame(rend->vg);
}

static void item_text_render(renderer_gl_t *rend, const item_t *item)
{
    int font = (item->text.effects & TEXT_BOLD) ? FONT_BOLD : FONT_REGULAR;
    nvgBeginFrame(rend->vg, rend->fb_size[0] / rend->scale,
                            rend->fb_size[1] / rend->scale, rend->scale);
    nvgSave(rend->vg);
    if (item->text.effects & TEXT_BLEND_ADD)
        nvgGlobalCompositeBlendFunc(rend->vg, NVG_ONE, NVG_ONE);
    nvgTranslate(rend->vg, item->text.pos[0], item->text.pos[1]);
    nvgRotate(rend->vg, item->text.angle);

    nvgFontFaceId(rend->vg, rend->fonts[font].id);

    if (sys_lang_supports_spacing() && item->text.effects & TEXT_SPACED)
        nvgTextLetterSpacing(rend->vg, round(item->text.size *
                             rend->fonts[font].scale * 0.2));
    if (sys_lang_supports_spacing() && item->text.effects & TEXT_SEMI_SPACED)
        nvgTextLetterSpacing(rend->vg, round(item->text.size *
                             rend->fonts[font].scale * 0.05));
    nvgFontSize(rend->vg, item->text.size * rend->fonts[font].scale);
    nvgFillColor(rend->vg, nvgRGBA(item->color[0] * 255,
                                   item->color[1] * 255,
                                   item->color[2] * 255,
                                   item->color[3] * 255));
    nvgTextAlign(rend->vg, item->text.align);
    nvgText(rend->vg, 0, 0, item->text.text, NULL);

    // Uncomment to see labels bounding box
    if ((0)) {
        float bounds[4];
        nvgTextBounds(rend->vg, 0, 0, item->text.text, NULL, bounds);
        nvgBeginPath(rend->vg);
        nvgRect(rend->vg, bounds[0],bounds[1], bounds[2]-bounds[0],
                bounds[3]-bounds[1]);
        nvgStrokeColor(rend->vg, nvgRGBA(item->color[0] * 255,
                       item->color[1] * 255,
                       item->color[2] * 255,
                       item->color[3] * 255));
        nvgStroke(rend->vg);
    }

    nvgRestore(rend->vg);
    nvgEndFrame(rend->vg);
}

static void item_fog_render(renderer_gl_t *rend, const item_t *item)
{
    gl_shader_t *shader;
    shader = shader_get("fog", NULL, ATTR_NAMES, init_shader);
    GL(glUseProgram(shader->prog));
    GL(glEnable(GL_CULL_FACE));
    GL(glCullFace(rend->cull_flipped ? GL_FRONT : GL_BACK));
    GL(glEnable(GL_BLEND));
    GL(glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                           GL_ZERO, GL_ONE));
    GL(glDisable(GL_DEPTH_TEST));
    draw_buffer(&item->buf, &item->indices, GL_TRIANGLES);
    GL(glCullFace(GL_BACK));
}

static void item_atmosphere_render(renderer_gl_t *rend, const item_t *item)
{
    gl_shader_t *shader;
    float tm[3];

    shader = shader_get("atmosphere", NULL, ATTR_NAMES, init_shader);
    GL(glUseProgram(shader->prog));

    GL(glActiveTexture(GL_TEXTURE0));
    GL(glBindTexture(GL_TEXTURE_2D, item->tex->id));
    GL(glEnable(GL_CULL_FACE));
    GL(glCullFace(rend->cull_flipped ? GL_FRONT : GL_BACK));

    GL(glEnable(GL_BLEND));
    if (color_is_white(item->color)) {
        GL(glBlendFunc(GL_ONE, GL_ONE));
    } else {
        GL(glBlendFunc(GL_CONSTANT_COLOR, GL_ONE));
        GL(glBlendColor(item->color[0] * item->color[3],
                        item->color[1] * item->color[3],
                        item->color[2] * item->color[3],
                        item->color[3]));
    }

    gl_update_uniform(shader, "u_color", item->color);
    gl_update_uniform(shader, "u_atm_p", item->atm.p);
    gl_update_uniform(shader, "u_sun", item->atm.sun);
    // XXX: the tonemapping args should be copied before rendering!
    tm[0] = core->tonemapper.p;
    tm[1] = core->tonemapper.lwmax;
    tm[2] = core->tonemapper.exposure;
    gl_update_uniform(shader, "u_tm", tm);
    draw_buffer(&item->buf, &item->indices, GL_TRIANGLES);
    GL(glCullFace(GL_BACK));
}

static void item_texture_render(renderer_gl_t *rend, const item_t *item)
{
    gl_shader_t *shader;

    shader_define_t defines[] = {
        {"TEXTURE_LUMINANCE", item->tex->format == GL_LUMINANCE &&
                              !(item->flags & PAINTER_ADD)},
        {}
    };
    shader = shader_get("blit", defines, ATTR_NAMES, init_shader);

    GL(glUseProgram(shader->prog));

    GL(glActiveTexture(GL_TEXTURE0));
    GL(glBindTexture(GL_TEXTURE_2D, item->tex->id));
    GL(glEnable(GL_CULL_FACE));
    GL(glCullFace(rend->cull_flipped ? GL_FRONT : GL_BACK));

    if (item->tex->format == GL_RGB && item->color[3] == 1.0) {
        GL(glDisable(GL_BLEND));
    } else {
        GL(glEnable(GL_BLEND));
        GL(glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                               GL_ZERO, GL_ONE));
    }
    GL(glDisable(GL_DEPTH_TEST));

    if (item->flags & PAINTER_ADD) {
        GL(glEnable(GL_BLEND));
        if (color_is_white(item->color))
            GL(glBlendFunc(GL_ONE, GL_ONE));
        else {
            GL(glBlendFunc(GL_CONSTANT_COLOR, GL_ONE));
            GL(glBlendColor(item->color[0] * item->color[3],
                            item->color[1] * item->color[3],
                            item->color[2] * item->color[3],
                            item->color[3]));
        }
    }

    gl_update_uniform(shader, "u_color", item->color);
    draw_buffer(&item->buf, &item->indices, GL_TRIANGLES);
    GL(glCullFace(GL_BACK));
}

static void item_quad_wireframe_render(renderer_gl_t *rend, const item_t *item)
{
    gl_shader_t *shader;

    shader = shader_get("blit", NULL, ATTR_NAMES, init_shader);
    GL(glUseProgram(shader->prog));

    gl_update_uniform(shader, "u_color", item->color);
    GL(glActiveTexture(GL_TEXTURE0));
    GL(glBindTexture(GL_TEXTURE_2D, rend->white_tex->id));
    GL(glDisable(GL_DEPTH_TEST));
    GL(glEnable(GL_BLEND));
    GL(glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                           GL_ZERO, GL_ONE));

    draw_buffer(&item->buf, &item->indices, GL_LINES);
}

static void item_planet_render(renderer_gl_t *rend, const item_t *item)
{
    gl_shader_t *shader;
    bool is_moon;
    const float depth_range[] = {rend->depth_range[0], rend->depth_range[1]};
    shader_define_t defines[] = {
        {"HAS_SHADOW", item->planet.shadow_spheres_nb > 0},
        {}
    };
    shader = shader_get("planet", defines, ATTR_NAMES, init_shader);

    GL(glUseProgram(shader->prog));

    GL(glActiveTexture(GL_TEXTURE0));
    GL(glBindTexture(GL_TEXTURE_2D, item->tex->id));

    GL(glActiveTexture(GL_TEXTURE1));
    if (item->planet.normalmap) {
        GL(glBindTexture(GL_TEXTURE_2D, item->planet.normalmap->id));
        gl_update_uniform(shader, "u_has_normal_tex", 1);
    } else {
        GL(glBindTexture(GL_TEXTURE_2D, rend->white_tex->id));
        gl_update_uniform(shader, "u_has_normal_tex", 0);
    }

    GL(glActiveTexture(GL_TEXTURE2));
    if (item->planet.shadow_color_tex &&
            texture_load(item->planet.shadow_color_tex, NULL))
        GL(glBindTexture(GL_TEXTURE_2D, item->planet.shadow_color_tex->id));
    else
        GL(glBindTexture(GL_TEXTURE_2D, rend->white_tex->id));

    if (item->flags & PAINTER_RING_SHADER) {
        GL(glDisable(GL_CULL_FACE));
    } else {
        GL(glEnable(GL_CULL_FACE));
        GL(glCullFace(rend->cull_flipped ? GL_FRONT : GL_BACK));
    }

    if (item->tex->format == GL_RGB && item->color[3] == 1.0) {
        GL(glDisable(GL_BLEND));
    } else {
        GL(glEnable(GL_BLEND));
        GL(glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                               GL_ZERO, GL_ONE));
    }
    if (item->depth_range[0] || item->depth_range[1]) {
        GL(glEnable(GL_DEPTH_TEST));
        GL(glDepthMask(GL_TRUE));
    }

    // Set all uniforms.
    is_moon = item->flags & PAINTER_IS_MOON;
    gl_update_uniform(shader, "u_color", item->color);
    gl_update_uniform(shader, "u_contrast", item->planet.contrast);
    gl_update_uniform(shader, "u_sun", item->planet.sun);
    gl_update_uniform(shader, "u_light_emit", item->planet.light_emit);
    gl_update_uniform(shader, "u_material", item->planet.material);
    gl_update_uniform(shader, "u_is_moon", is_moon ? 1 : 0);
    gl_update_uniform(shader, "u_mv", item->planet.mv);
    gl_update_uniform(shader, "u_shadow_spheres_nb",
                      item->planet.shadow_spheres_nb);
    gl_update_uniform(shader, "u_shadow_spheres", item->planet.shadow_spheres);
    gl_update_uniform(shader, "u_tex_transf", item->planet.tex_transf);
    gl_update_uniform(shader, "u_normal_tex_transf",
                      item->planet.normal_tex_transf);
    gl_update_uniform(shader, "u_depth_range", depth_range);

    draw_buffer(&item->buf, &item->indices, GL_TRIANGLES);
    GL(glCullFace(GL_BACK));
    GL(glDepthMask(GL_FALSE));
    GL(glDisable(GL_DEPTH_TEST));
}

static void item_gltf_render(renderer_gl_t *rend, const item_t *item)
{
    double proj[4][4], nearval, farval;
    mat4_copy(item->gltf.proj_mat, proj);

    if (item->depth_range[0]) {
        // Fix the depth range of the projection to the current frame values.
        nearval = rend->depth_range[0] * DAU;
        farval = rend->depth_range[1] * DAU;
        proj[2][2] = (farval + nearval) / (nearval - farval);
        proj[3][2] = 2. * farval * nearval / (nearval - farval);
    }

    gltf_render(item->gltf.model, item->gltf.model_mat, item->gltf.view_mat,
                proj, item->gltf.light_dir, item->gltf.args);
}

static void rend_flush(renderer_gl_t *rend)
{
    item_t *item, *tmp;

    // Compute depth range.
    rend->depth_range[0] = DBL_MAX;
    rend->depth_range[1] = DBL_MIN;
    DL_FOREACH(rend->items, item) {
        if (item->depth_range[0] || item->depth_range[1]) {
            rend->depth_range[0] = min(rend->depth_range[0],
                                       item->depth_range[0]);
            rend->depth_range[1] = max(rend->depth_range[1],
                                       item->depth_range[1]);
        }
    }
    if (rend->depth_range[0] == DBL_MAX) {
        rend->depth_range[0] = 0;
        rend->depth_range[1] = 1;
    }

    // Set default OpenGL state.
    GL(glClearColor(0.0, 0.0, 0.0, 1.0));
    GL(glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT));
    GL(glViewport(0, 0, rend->fb_size[0], rend->fb_size[1]));
    GL(glDepthMask(GL_FALSE));
    GL(glDisable(GL_DEPTH_TEST));

    // On OpenGL Desktop, we have to enable point sprite support.
#ifndef GLES2
    GL(glEnable(GL_PROGRAM_POINT_SIZE));
    GL(glEnable(GL_POINT_SPRITE));
#endif

    DL_FOREACH_SAFE(rend->items, item, tmp) {
        switch (item->type) {
        case ITEM_LINES:
            item_lines_render(rend, item);
            break;
        case ITEM_LINES_GLOW:
            item_lines_glow_render(rend, item);
            break;
        case ITEM_MESH:
            item_mesh_render(rend, item);
            break;
        case ITEM_POINTS:
            item_points_render(rend, item);
            break;
        case ITEM_TEXTURE:
            item_texture_render(rend, item);
            break;
        case ITEM_ATMOSPHERE:
            item_atmosphere_render(rend, item);
            break;
        case ITEM_FOG:
            item_fog_render(rend, item);
            break;
        case ITEM_PLANET:
            item_planet_render(rend, item);
            break;
        case ITEM_VG_ELLIPSE:
        case ITEM_VG_RECT:
        case ITEM_VG_LINE:
            item_vg_render(rend, item);
            break;
        case ITEM_TEXT:
            item_text_render(rend, item);
            break;
        case ITEM_QUAD_WIREFRAME:
            item_quad_wireframe_render(rend, item);
            break;
        case ITEM_GLTF:
            item_gltf_render(rend, item);
            break;
        default:
            assert(false);
        }

        DL_DELETE(rend->items, item);
        texture_release(item->tex);
        if (item->type == ITEM_PLANET)
            texture_release(item->planet.normalmap);
        if (item->type == ITEM_GLTF)
            json_builder_free(item->gltf.args);
        gl_buf_release(&item->buf);
        gl_buf_release(&item->indices);
        free(item);
    }
    // Reset to default OpenGL settings.
    GL(glDepthMask(GL_TRUE));
}

static void finish(renderer_t *rend_)
{
    renderer_gl_t *rend = (void*)rend_;
    rend_flush(rend);
}

static void line_glow(renderer_t           *rend_,
                      const painter_t      *painter,
                      const double         (*line)[3],
                      int                  size)
{
    renderer_gl_t *rend = (void*)rend_;
    line_mesh_t *mesh;
    int i, ofs;
    float color[4];
    item_t *item;

    vec4_to_float(painter->color, color);
    mesh = line_to_mesh(line, size, 10);

    if (mesh->indices_count >= 1024 || mesh->verts_count >= 1024) {
        LOG_W("Too many points in lines! (size: %d)", size);
        goto end;
    }

    // Get the item.
    item = get_item(rend, ITEM_LINES_GLOW, mesh->verts_count,
                    mesh->indices_count, NULL);
    if (item && memcmp(item->color, color, sizeof(color))) item = NULL;
    if (item && item->lines.dash_length != painter->lines.dash_length)
        item = NULL;
    if (item && item->lines.dash_ratio != painter->lines.dash_ratio)
        item = NULL;
    if (item && item->lines.width != painter->lines.width)
        item = NULL;


    if (!item) {
        item = calloc(1, sizeof(*item));
        item->type = ITEM_LINES_GLOW;
        gl_buf_alloc(&item->buf, &LINES_GLOW_BUF, 1024);
        gl_buf_alloc(&item->indices, &INDICES_BUF, 1024);
        item->lines.width = painter->lines.width;
        item->lines.glow = painter->lines.glow;
        item->lines.dash_length = painter->lines.dash_length;
        item->lines.dash_ratio = painter->lines.dash_ratio;
        item->lines.fade_dist_min = painter->lines.fade_dist_min;
        item->lines.fade_dist_max = painter->lines.fade_dist_max;
        memcpy(item->color, color, sizeof(color));
        DL_APPEND(rend->items, item);
    }
    if (painter->depth_range)
        vec2_to_float(*painter->depth_range, item->depth_range);

    // Append the mesh to the buffer.
    ofs = item->buf.nb;
    for (i = 0; i < mesh->verts_count; i++) {
        gl_buf_3f(&item->buf, -1, ATTR_POS, VEC3_SPLIT(mesh->verts[i].pos));
        gl_buf_2f(&item->buf, -1, ATTR_TEX_POS, VEC2_SPLIT(mesh->verts[i].uv));
        gl_buf_next(&item->buf);
    }
    for (i = 0; i < mesh->indices_count; i++) {
        gl_buf_1i(&item->indices, -1, 0, mesh->indices[i] + ofs);
        gl_buf_next(&item->indices);
    }

end:
    line_mesh_delete(mesh);
}

static void line(renderer_t           *rend_,
                 const painter_t      *painter,
                 const double         (*line)[3],
                 int                  size)
{
    int i, ofs;
    renderer_gl_t *rend = (void*)rend_;
    item_t *item;
    float color[4];
    double pos[2];

    if (painter->lines.glow) {
        line_glow(rend_, painter, line, size);
        return;
    }

    vec4_to_float(painter->color, color);
    item = get_item(rend, ITEM_LINES, size, size * 2, NULL);
    if (item && memcmp(item->color, color, sizeof(color))) item = NULL;
    if (item && item->lines.width != painter->lines.width) item = NULL;

    if (!item) {
        item = calloc(1, sizeof(*item));
        item->type = ITEM_LINES;
        gl_buf_alloc(&item->buf, &LINES_BUF, 1024);
        gl_buf_alloc(&item->indices, &INDICES_BUF, 1024);
        item->lines.width = painter->lines.width;
        memcpy(item->color, color, sizeof(color));
        DL_APPEND(rend->items, item);
    }

    ofs = item->buf.nb;

    for (i = 0; i < size; i++) {
        window_to_ndc(rend, line[i], pos);
        gl_buf_2f(&item->buf, -1, ATTR_TEX_POS, (double)i / (size - 1), 0);
        gl_buf_4f(&item->buf, -1, ATTR_POS, VEC2_SPLIT(pos), 0.0, 1.0);
        gl_buf_4i(&item->buf, -1, ATTR_COLOR, 255, 255, 255, 255);
        gl_buf_next(&item->buf);
        if (i < size - 1) {
            gl_buf_1i(&item->indices, -1, 0, ofs + i);
            gl_buf_next(&item->indices);
            gl_buf_1i(&item->indices, -1, 0, ofs + i + 1);
            gl_buf_next(&item->indices);
        }
    }
}

static void mesh(renderer_t          *rend_,
                 const painter_t     *painter,
                 int                 frame,
                 int                 mode,
                 int                 verts_count,
                 const double        verts[][3],
                 int                 indices_count,
                 const uint16_t      indices[],
                 bool                use_stencil)
{
    int i, ofs;
    double pos[4] = {};
    double rot[3][3];
    uint8_t color[4];
    item_t *item;
    renderer_gl_t *rend = (void*)rend_;

    color[0] = painter->color[0] * 255;
    color[1] = painter->color[1] * 255;
    color[2] = painter->color[2] * 255;
    color[3] = painter->color[3] * 255;
    if (!color[3]) return;

    item = get_item(rend, ITEM_MESH, verts_count, indices_count, NULL);
    if (item && (use_stencil != item->mesh.use_stencil)) item = NULL;
    if (item && item->mesh.mode != mode) item = NULL;
    if (item && item->mesh.stroke_width != painter->lines.width) item = NULL;

    if (!item) {
        item = calloc(1, sizeof(*item));
        item->type = ITEM_MESH;
        item->mesh.mode = mode;
        item->mesh.stroke_width = painter->lines.width;
        item->mesh.use_stencil = use_stencil;
        gl_buf_alloc(&item->buf, &MESH_BUF, max(verts_count, 1024));
        gl_buf_alloc(&item->indices, &INDICES_BUF, max(indices_count, 1024));
        DL_APPEND(rend->items, item);
    }

    ofs = item->buf.nb;
    // Project the vertices.

    // Special cas for Mollweide projection when the frame convertion can
    // be expressed as a single matrix rotation.
    if ((painter->proj->klass->id == PROJ_MOLLWEIDE ||
         painter->proj->klass->id == PROJ_MOLLWEIDE_ADAPTIVE) &&
            frame_get_rotation(painter->obs, frame, FRAME_VIEW, rot)) {
        item->mesh.proj = PROJ_MOLLWEIDE;
        vec2_to_float(painter->proj->scaling, item->mesh.proj_scaling);
        for (i = 0; i < verts_count; i++) {
            mat3_mul_vec3(rot, verts[i], pos);
            gl_buf_4f(&item->buf, -1, ATTR_POS, VEC4_SPLIT(pos));
            gl_buf_4i(&item->buf, -1, ATTR_COLOR, VEC4_SPLIT(color));
            gl_buf_next(&item->buf);
        }
    } else {
        for (i = 0; i < verts_count; i++) {
            vec3_copy(verts[i], pos);
            pos[3] = 0.0;
            vec3_normalize(pos, pos);
            convert_frame(painter->obs, frame, FRAME_VIEW, true, pos, pos);
            pos[3] = 0.0;
            project(painter->proj, PROJ_ALREADY_NORMALIZED, pos, pos);
            gl_buf_4f(&item->buf, -1, ATTR_POS, VEC4_SPLIT(pos));
            gl_buf_4i(&item->buf, -1, ATTR_COLOR, VEC4_SPLIT(color));
            gl_buf_next(&item->buf);
        }
    }

    // Fill the indice buffer.
    for (i = 0; i < indices_count; i++) {
        gl_buf_1i(&item->indices, -1, 0, indices[i] + ofs);
        gl_buf_next(&item->indices);
    }
}

static void ellipse_2d(renderer_t *rend_, const painter_t *painter,
                       const double pos[2], const double size[2],
                       double angle, double dashes)
{
    renderer_gl_t *rend = (void*)rend_;
    item_t *item;
    item = calloc(1, sizeof(*item));
    item->type = ITEM_VG_ELLIPSE;
    vec2_to_float(pos, item->vg.pos);
    vec2_to_float(size, item->vg.size);
    vec4_to_float(painter->color, item->color);
    item->vg.angle = angle;
    item->vg.dashes = dashes;
    item->vg.stroke_width = painter->lines.width;
    DL_APPEND(rend->items, item);
}

static void rect_2d(renderer_t *rend_, const painter_t *painter,
                    const double pos[2], const double size[2], double angle)
{
    renderer_gl_t *rend = (void*)rend_;
    item_t *item;
    item = calloc(1, sizeof(*item));
    item->type = ITEM_VG_RECT;
    vec2_to_float(pos, item->vg.pos);
    vec2_to_float(size, item->vg.size);
    vec4_to_float(painter->color, item->color);
    item->vg.angle = angle;
    item->vg.stroke_width = painter->lines.width;
    DL_APPEND(rend->items, item);
}

static void line_2d(renderer_t *rend_, const painter_t *painter,
                    const double p1[2], const double p2[2])
{
    renderer_gl_t *rend = (void*)rend_;
    item_t *item;
    item = calloc(1, sizeof(*item));
    item->type = ITEM_VG_LINE;
    vec2_to_float(p1, item->vg.pos);
    vec2_to_float(p2, item->vg.pos2);
    vec4_to_float(painter->color, item->color);
    item->vg.stroke_width = painter->lines.width;
    DL_APPEND(rend->items, item);
}

static void model_3d(renderer_t *rend_, const painter_t *painter,
                     const char *model,
                     const double model_mat[4][4],
                     const double view_mat[4][4],
                     const double proj_mat[4][4],
                     const double light_dir[3],
                     json_value *args)
{
    renderer_gl_t *rend = (void*)rend_;
    item_t *item;
    item = calloc(1, sizeof(*item));
    item->type = ITEM_GLTF;
    item->gltf.model = model;
    mat4_copy(model_mat, item->gltf.model_mat);
    mat4_copy(view_mat, item->gltf.view_mat);
    mat4_copy(proj_mat, item->gltf.proj_mat);
    vec3_copy(light_dir, item->gltf.light_dir);
    if (painter->depth_range)
        vec2_to_float(*painter->depth_range, item->depth_range);
    if (args) item->gltf.args = json_copy(args);
    DL_APPEND(rend->items, item);
}

static texture_t *create_white_texture(int w, int h)
{
    uint8_t *data;
    texture_t *tex;
    data = malloc(w * h * 3);
    memset(data, 255, w * h * 3);
    tex = texture_from_data(data, w, h, 3, 0, 0, w, h, 0);
    free(data);
    return tex;
}

EMSCRIPTEN_KEEPALIVE
void core_add_font(renderer_gl_t *rend, const char *name,
                   const char *url, const uint8_t *data,
                   int size, float scale)
{
    int id;
    int font;
    rend = rend ?: (void*)core->rend;

    if (!data) {
        data = asset_get_data(url, &size, NULL);
        assert(data);
    }

    if (strcmp(name, "regular") == 0)
        font = FONT_REGULAR;
    else if (strcmp(name, "bold") == 0)
        font = FONT_BOLD;
    else {
        assert(false);
        return;
    }

    id = nvgCreateFontMem(rend->vg, name, data, size, 0);
    if (!rend->fonts[font].id || rend->fonts[font].is_default_font) {
        rend->fonts[font].id = id;
        rend->fonts[font].scale = scale;
        rend->fonts[font].is_default_font = false;
    } else {
        nvgAddFallbackFontId(rend->vg, rend->fonts[font].id, id);
    }
}

static void set_default_fonts(renderer_gl_t *rend)
{
    const float scale = 1.38;
    core_add_font(rend, "regular", "asset://font/NotoSans-Regular.ttf",
                  NULL, 0, scale);
    core_add_font(rend, "bold", "asset://font/NotoSans-Bold.ttf",
                  NULL, 0, scale);
    rend->fonts[FONT_REGULAR].is_default_font = true;
    rend->fonts[FONT_BOLD].is_default_font = true;
}

renderer_t* render_gl_create(void)
{
    renderer_gl_t *rend;
    GLint range[2];

#ifdef WIN32
    glewInit();
#endif

    rend = calloc(1, sizeof(*rend));
    rend->white_tex = create_white_texture(16, 16);
#ifdef GLES2
    rend->vg = nvgCreateGLES2(NVG_ANTIALIAS);
#else
    rend->vg = nvgCreateGL2(NVG_ANTIALIAS);
#endif

    if (!sys_callbacks.render_text)
        set_default_fonts(rend);

    // Query the point size range.
    GL(glGetIntegerv(GL_ALIASED_POINT_SIZE_RANGE, range));
    if (range[1] < 32)
        LOG_W("OpenGL Doesn't support large point size!");

    rend->rend.prepare = prepare;
    rend->rend.finish = finish;
    rend->rend.points_2d = points_2d;
    rend->rend.quad = quad;
    rend->rend.quad_wireframe = quad_wireframe;
    rend->rend.texture = texture;
    rend->rend.text = text;
    rend->rend.line = line;
    rend->rend.mesh = mesh;
    rend->rend.ellipse_2d = ellipse_2d;
    rend->rend.rect_2d = rect_2d;
    rend->rend.line_2d = line_2d;
    rend->rend.model_3d = model_3d;

    return &rend->rend;
}
