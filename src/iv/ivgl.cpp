// Copyright Contributors to the OpenImageIO project.
// SPDX-License-Identifier: BSD-3-Clause and Apache-2.0
// https://github.com/AcademySoftwareFoundation/OpenImageIO

#include "ivgl.h"
#include "imageviewer.h"

#include <iostream>
#include <limits>

#include <QComboBox>
#include <QFontDatabase>
#include <QLabel>
#include <QMouseEvent>
#include <QProgressBar>
#if OIIO_QT_MAJOR >= 6
#    include <QPainter>
#    include <QPen>
#endif

#include "ivutils.h"
#include <OpenImageIO/strutil.h>
#include <OpenImageIO/timer.h>
#include <cfloat>

OIIO_PRAGMA_WARNING_PUSH
#if defined(__APPLE__)
// Apple deprecates OpenGL calls, ugh
OIIO_CLANG_PRAGMA(GCC diagnostic ignored "-Wdeprecated-declarations")
#endif



static const char*
gl_err_to_string(GLenum err)
{  // Thanks, Dan Wexler, for this function
    switch (err) {
    case GL_NO_ERROR: return "No error";
    case GL_INVALID_ENUM: return "Invalid enum";
    case GL_INVALID_OPERATION: return "Invalid operation";
    case GL_INVALID_VALUE: return "Invalid value";
    case GL_OUT_OF_MEMORY: return "Out of memory";
    case GL_INVALID_FRAMEBUFFER_OPERATION:
        return "Invalid framebuffer operation";
    default: return "Unknown";
    }
}

IvGL::IvGL(QWidget* parent, ImageViewer& viewer)
    : QOpenGLWidget(parent)
    , m_viewer(viewer)
    , m_shaders_created(false)
    , m_vertex_shader(0)
    , m_shader_program(0)
    , m_tex_created(false)
    , m_zoom(1.0)
    , m_centerx(0)
    , m_centery(0)
    , m_dragging(false)
    , m_mousex(0)
    , m_mousey(0)
    , m_drag_button(Qt::NoButton)
    , m_use_shaders(false)
    , m_use_halffloat(false)
    , m_use_float(false)
    , m_use_srgb(false)
    , m_texture_width(1)
    , m_texture_height(1)
    , m_last_pbo_used(0)
    , m_current_image(NULL)
    , m_pixelview_left_corner(true)
    , m_last_texbuf_used(0)
{
#if 0
    QGLFormat format;
    format.setRedBufferSize (32);
    format.setGreenBufferSize (32);
    format.setBlueBufferSize (32);
    format.setAlphaBufferSize (32);
    format.setDepth (true);
    setFormat (format);
#endif
    m_mouse_activation = false;
    this->setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
}



IvGL::~IvGL() {}



void
IvGL::initializeGL()
{
    initializeOpenGLFunctions();

    glClearColor(0.05f, 0.05f, 0.05f, 1.0f);
    glEnable(GL_BLEND);
    glEnable(GL_TEXTURE_2D);
    // glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    // Make sure initial matrix is identity (returning to this stack level loads
    // back this matrix).
    glLoadIdentity();

#if 1
    // Compensate for high res displays with device pixel ratio scaling
    float dpr = m_viewer.devicePixelRatio();
    glScalef(dpr, dpr, 1.0f);
#endif

    // There's this small detail in the OpenGL 2.1 (probably earlier versions
    // too) spec:
    //
    // (For TexImage3D, TexImage2D and TexImage1D):
    // The values of UNPACK ROW LENGTH and UNPACK ALIGNMENT control the row-to-
    // row spacing in these images in the same manner as DrawPixels.
    //
    // UNPACK_ALIGNMENT has a default value of 4 according to the spec. Which
    // means that it was expecting images to be Aligned to 4-bytes, and making
    // several odd "skew-like effects" in the displayed images. Setting the
    // alignment to 1 byte fixes this problems.
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    // here we check what OpenGL extensions are available, and take action
    // if needed
    check_gl_extensions();

    create_textures();

    create_shaders();
}



void
IvGL::create_textures(void)
{
    if (m_tex_created)
        return;

    // FIXME: Determine this dynamically.
    const int total_texbufs = 4;
    GLuint textures[total_texbufs];

    glGenTextures(total_texbufs, textures);

    // Initialize texture objects
    for (unsigned int texture : textures) {
        m_texbufs.emplace_back();
        glBindTexture(GL_TEXTURE_2D, texture);
        print_error("bind tex");
        glTexImage2D(GL_TEXTURE_2D, 0 /*mip level*/,
                     4 /*internal format - color components */, 1 /*width*/,
                     1 /*height*/, 0 /*border width*/,
                     GL_RGBA /*type - GL_RGB, GL_RGBA, GL_LUMINANCE */,
                     GL_FLOAT /*format - GL_FLOAT */, NULL /*data*/);
        print_error("tex image 2d");
        // Initialize tex parameters.
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
        print_error("After tex parameters");
        m_texbufs.back().tex_object = texture;
        m_texbufs.back().x          = 0;
        m_texbufs.back().y          = 0;
        m_texbufs.back().width      = 0;
        m_texbufs.back().height     = 0;
    }

    // Create another texture for the pixelview.
    glGenTextures(1, &m_pixelview_tex);
    glBindTexture(GL_TEXTURE_2D, m_pixelview_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, 4, closeup_texture_size,
                 closeup_texture_size, 0, GL_RGBA, GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glGenBuffers(2, m_pbo_objects);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, m_pbo_objects[0]);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, m_pbo_objects[1]);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

    m_tex_created = true;
}

const char*
IvGL::color_func_shader_text()
{
    return R"glsl(
        uniform float gain;
        uniform float gamma;

        vec4 ColorFunc(vec4 C)
        {
            C.xyz *= gain;
            float invgamma = 1.0/gamma;
            C.xyz = pow (C.xyz, vec3 (invgamma, invgamma, invgamma));
            return C;
        }
    )glsl";
}

void
IvGL::create_shaders(void)
{
    if (!m_use_shaders) {
        std::cerr << "Not using shaders!\n";
        return;
    }

    const char* color_shader = color_func_shader_text();
    if (m_color_shader_text != color_shader) {
        if (m_shader_program) {
            if (m_vertex_shader) {
                glDetachShader(m_shader_program, m_vertex_shader);
            }
            glUseProgram(0);
            glDeleteProgram(m_shader_program);
            m_shader_program  = 0;
            m_shaders_created = false;
        }
    }

    if (m_shaders_created) {
        return;
    }

    // This holds the compilation status
    GLint status;

    if (!m_vertex_shader) {
        static const GLchar* vertex_source = R"glsl(
            varying vec2 vTexCoord;
            void main ()
            {
                vTexCoord = gl_MultiTexCoord0.xy;
                gl_Position = ftransform();
            }
        )glsl";

        m_vertex_shader = glCreateShader(GL_VERTEX_SHADER);
        glShaderSource(m_vertex_shader, 1, &vertex_source, NULL);
        glCompileShader(m_vertex_shader);
        glGetShaderiv(m_vertex_shader, GL_COMPILE_STATUS, &status);

        if (!status) {
            std::cerr << "vertex shader compile status: " << status << "\n";
            print_shader_log(std::cerr, m_vertex_shader);
            create_shaders_abort();
            return;
        }
    }

    static const GLchar* fragment_source = R"glsl(
        uniform sampler2D imgtex;
        varying vec2 vTexCoord;
        uniform int startchannel;
        uniform int colormode;
        // Remember, if imgchannels == 2, second channel would be channel 4 (a).
        uniform int imgchannels;
        uniform int pixelview;
        uniform int linearinterp;
        uniform int width;
        uniform int height;

        vec4 rgba_mode (vec4 C)
        {
            if (imgchannels <= 2) {
                if (startchannel == 1)
                return vec4(C.aaa, 1.0);
                return C.rrra;
            }
            return C;
        }

        vec4 rgb_mode (vec4 C)
        {
            if (imgchannels <= 2) {
                if (startchannel == 1)
                return vec4(C.aaa, 1.0);
                return vec4 (C.rrr, 1.0);
            }
            float C2[4];
            C2[0]=C.x; C2[1]=C.y; C2[2]=C.z; C2[3]=C.w;
            return vec4 (C2[startchannel], C2[startchannel+1], C2[startchannel+2], 1.0);
        }

        vec4 singlechannel_mode (vec4 C)
        {
            float C2[4];
            C2[0]=C.x; C2[1]=C.y; C2[2]=C.z; C2[3]=C.w;
            if (startchannel > imgchannels)
                return vec4 (0.0,0.0,0.0,1.0);
            return vec4 (C2[startchannel], C2[startchannel], C2[startchannel], 1.0);
        }

        vec4 luminance_mode (vec4 C)
        {
            if (imgchannels <= 2)
                return vec4 (C.rrr, C.a);
            float lum = dot (C.rgb, vec3(0.2126, 0.7152, 0.0722));
            return vec4 (lum, lum, lum, C.a);
        }

        float heat_red(float x)
        {
            return clamp (mix(0.0, 1.0, (x-0.35)/(0.66-0.35)), 0.0, 1.0) -
                clamp (mix(0.0, 0.5, (x-0.89)/(1.0-0.89)), 0.0, 1.0);
        }

        float heat_green(float x)
        {
            return clamp (mix(0.0, 1.0, (x-0.125)/(0.375-0.125)), 0.0, 1.0) -
                clamp (mix(0.0, 1.0, (x-0.64)/(0.91-0.64)), 0.0, 1.0);
        }

        vec4 heatmap_mode (vec4 C)
        {
            float C2[4];
            C2[0]=C.x; C2[1]=C.y; C2[2]=C.z; C2[3]=C.w;
            return vec4(heat_red(C2[startchannel]),
                        heat_green(C2[startchannel]),
                        heat_red(1.0-C2[startchannel]),
                        1.0);
        }

        void main ()
        {
            vec2 st = vTexCoord;
            float black = 0.0;
            if (pixelview != 0 || linearinterp == 0) {
                vec2 wh = vec2(width,height);
                vec2 onehalf = vec2(0.5,0.5);
                vec2 st_res = st * wh /* + onehalf */ ;
                vec2 st_pix = floor (st_res);
                vec2 st_rem = st_res - st_pix;
                st = (st_pix + onehalf) / wh;
                if (pixelview != 0) {
                    if (st.x < 0.0 || st.x >= 1.0 || 
                            st.y < 0.0 || st.y >= 1.0 || 
                            st_rem.x < 0.05 || st_rem.x >= 0.95 || 
                            st_rem.y < 0.05 || st_rem.y >= 0.95)
                        black = 1.0;
                }
            }
            vec4 C = texture2D (imgtex, st);
            C = mix (C, vec4(0.05,0.05,0.05,1.0), black);
            if (startchannel < 0)
                C = vec4(0.0,0.0,0.0,1.0);
            else if (colormode == 0) // RGBA
                C = rgba_mode (C);
            else if (colormode == 1) // RGB (i.e., ignore alpha).
                C = rgb_mode (C);
            else if (colormode == 2) // Single channel.
                C = singlechannel_mode (C);
            else if (colormode == 3) // Luminance.
                C = luminance_mode (C);
            else if (colormode == 4) // Heatmap.
                C = heatmap_mode (C);
            if (pixelview != 0)
                C.a = 1.0;
            C = ColorFunc(C);
            gl_FragColor = C;
        }
    )glsl";

    const char* fragment_sources[] = { "#version 120\n", color_shader,
                                       fragment_source };
    m_color_shader_text            = color_shader;

    GLuint fragment_shader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragment_shader, 3, fragment_sources, NULL);
    glCompileShader(fragment_shader);
    glGetShaderiv(fragment_shader, GL_COMPILE_STATUS, &status);
    if (!status) {
        std::cerr << "fragment shader compile status: " << status << "\n";
        print_shader_log(std::cerr, fragment_shader);
        create_shaders_abort();
        return;
    }

    if (!m_shader_program) {
        // When using extensions to support shaders, we need to load the
        // function entry points (which is actually done by GLEW) and then call
        // them. So we have to get the functions through the right symbols
        // otherwise extension-based shaders won't work.
        m_shader_program = glCreateProgram();
        print_error("create program");

        glAttachShader(m_shader_program, m_vertex_shader);
        print_error("After attach vertex shader.");

        glAttachShader(m_shader_program, fragment_shader);
        print_error("After attach fragment shader");

        glLinkProgram(m_shader_program);
        print_error("link");
        GLint linked;
        glGetProgramiv(m_shader_program, GL_LINK_STATUS, &linked);
        if (!linked) {
            std::cerr << "NOT LINKED\n";
            char buf[10000];
            buf[0] = 0;
            GLsizei len;
            glGetProgramInfoLog(m_shader_program, sizeof(buf), &len, buf);
            std::cerr << "link log:\n" << buf << "---\n";
            create_shaders_abort();
            return;
        }

        glDetachShader(m_shader_program, fragment_shader);
        print_error("After detach fragment shader");

        glDeleteShader(fragment_shader);
        print_error("After delete fragment shader");
    }

    m_shaders_created = true;
}



void
IvGL::create_shaders_abort(void)
{
    glUseProgram(0);
    if (m_shader_program)
        glDeleteProgram(m_shader_program);
    if (m_vertex_shader)
        glDeleteShader(m_vertex_shader);

    print_error("After delete shaders");
    m_use_shaders = false;
}



void
IvGL::resizeGL(int w, int h)
{
    print_error("resizeGL entry");
    glViewport(0, 0, w, h);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(-w / 2.0, w / 2.0, -h / 2.0, h / 2.0, 0, 10);
    // Main GL viewport is set up for orthographic view centered at
    // (0,0) and with width and height equal to the window dimensions IN
    // PIXEL UNITS.
    glMatrixMode(GL_MODELVIEW);

    clamp_view_to_window();
    print_error("resizeGL exit");
}



static void
gl_rect(float xmin, float ymin, float xmax, float ymax, float z = 0,
        float smin = 0, float tmin = 0, float smax = 1, float tmax = 1,
        int rotate = 0)
{
    float tex[] = { smin, tmin, smax, tmin, smax, tmax, smin, tmax };
    glBegin(GL_POLYGON);
    glTexCoord2f(tex[(0 + 2 * rotate) & 7], tex[(1 + 2 * rotate) & 7]);
    glVertex3f(xmin, ymin, z);
    glTexCoord2f(tex[(2 + 2 * rotate) & 7], tex[(3 + 2 * rotate) & 7]);
    glVertex3f(xmax, ymin, z);
    glTexCoord2f(tex[(4 + 2 * rotate) & 7], tex[(5 + 2 * rotate) & 7]);
    glVertex3f(xmax, ymax, z);
    glTexCoord2f(tex[(6 + 2 * rotate) & 7], tex[(7 + 2 * rotate) & 7]);
    glVertex3f(xmin, ymax, z);
    glEnd();
}



static void
gl_rect_border(float xmin, float ymin, float xmax, float ymax, float z = 0)
{
    glBegin(GL_LINE_LOOP);
    glVertex3f(xmin, ymin, z);
    glVertex3f(xmax, ymin, z);
    glVertex3f(xmax, ymax, z);
    glVertex3f(xmin, ymax, z);
    glEnd();
}



static void
gl_rect_dotted_border(float xmin, float ymin, float xmax, float ymax,
                      float z = 0)
{
    glPushAttrib(GL_ENABLE_BIT);
    glLineStipple(1, 0xF0F0);
    glEnable(GL_LINE_STIPPLE);
    gl_rect_border(xmin, ymin, xmax, ymax, z);
    glPopAttrib();
}



static void
handle_orientation(int orientation, int width, int height, float& scale_x,
                   float& scale_y, float& rotate_z, float& point_x,
                   float& point_y, bool pixel = false)
{
    switch (orientation) {
    case 2:  // flipped horizontally
        scale_x = -1;
        point_x = width - point_x;
        if (pixel)
            // We want to access the pixel at (point_x,pointy), so we have to
            // subtract 1 to get the right index.
            --point_x;
        break;
    case 3:  // bottom up, right to left (rotated 180).
        scale_x = -1;
        scale_y = -1;
        point_x = width - point_x;
        point_y = height - point_y;
        if (pixel) {
            --point_x;
            --point_y;
        }
        break;
    case 4:  // flipped vertically.
        scale_y = -1;
        point_y = height - point_y;
        if (pixel)
            --point_y;
        break;
    case 5:  // transposed (flip horizontal & rotated 90 ccw).
        scale_x  = -1;
        rotate_z = 90.0;
        std::swap(point_x, point_y);
        break;
    case 6:  // rotated 90 cw.
        rotate_z = -270.0;
        std::swap(point_x, point_y);
        point_y = height - point_y;
        if (pixel)
            --point_y;
        break;
    case 7:  // transverse, (flip horizontal & rotated 90 cw, r-to-l, b-to-t)
        scale_x  = -1;
        rotate_z = -90.0;
        std::swap(point_x, point_y);
        point_x = width - point_x;
        point_y = height - point_y;
        if (pixel) {
            --point_x;
            --point_y;
        }
        break;
    case 8:  // rotated 90 ccw.
        rotate_z = -90.0;
        std::swap(point_x, point_y);
        point_x = width - point_x;
        if (pixel)
            --point_x;
        break;
    case 1:  // horizontal
    case 0:  // unknown
    default: break;
    }
}



void
IvGL::paintGL()
{
#ifndef NDEBUG
    Timer paint_image_time;
    paint_image_time.start();
#endif
    //std::cerr << "paintGL " << m_viewer.current_image() << " with zoom " << m_zoom << "\n";
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    IvImage* img = m_current_image;
    if (!img || !img->image_valid())
        return;

    const ImageSpec& spec(img->spec());
    float z = m_zoom;

    glPushMatrix();
    glLoadIdentity();
    // Transform is now same as the main GL viewport -- window pixels as
    // units, with (0,0) at the center of the visible unit.
    glTranslatef(0, 0, -5);
    // Pushed away from the camera 5 units.
    glScalef(1, -1, 1);
    // Flip y, because OGL's y runs from bottom to top.
    glScalef(z, z, 1);
    // Scaled by zoom level.  So now xy units are image pixels as
    // displayed at the current zoom level, with the origin at the
    // center of the visible window.

    // Handle the orientation with OpenGL *before* translating our center.
    float scale_x      = 1;
    float scale_y      = 1;
    float rotate_z     = 0;
    float real_centerx = m_centerx;
    float real_centery = m_centery;
    handle_orientation(img->orientation(), spec.width, spec.height, scale_x,
                       scale_y, rotate_z, real_centerx, real_centery);

    glScalef(scale_x, scale_y, 1);
    glRotatef(rotate_z, 0, 0, 1);
    glTranslatef(-real_centerx, -real_centery, 0.0f);
    // Recentered so that the pixel space (m_centerx,m_centery) position is
    // at the center of the visible window.

    update_state();

    useshader(m_texture_width, m_texture_height);

    float smin = 0, smax = 1.0;
    float tmin = 0, tmax = 1.0;
    // Image pixels shown from the center to the edge of the window.
    int wincenterx = (int)ceil(width() / (2 * m_zoom));
    int wincentery = (int)ceil(height() / (2 * m_zoom));
    if (img->orientation() > 4) {
        std::swap(wincenterx, wincentery);
    }

    int xbegin = (int)floor(real_centerx) - wincenterx;
    xbegin     = std::max(spec.x, xbegin - (xbegin % m_texture_width));
    int ybegin = (int)floor(real_centery) - wincentery;
    ybegin     = std::max(spec.y, ybegin - (ybegin % m_texture_height));
    int xend   = (int)floor(real_centerx) + wincenterx;
    xend       = std::min(spec.x + spec.width,
                          xend + m_texture_width - (xend % m_texture_width));
    int yend   = (int)floor(real_centery) + wincentery;
    yend       = std::min(spec.y + spec.height,
                          yend + m_texture_height - (yend % m_texture_height));
    //std::cerr << "(" << xbegin << ',' << ybegin << ") - (" << xend << ',' << yend << ")\n";

    // Provide some feedback
    m_viewer.statusViewInfo->hide();
    m_viewer.statusProgress->show();

    // FIXME: change the code path so we can take full advantage of async DMA
    // when using PBO.
    for (int ystart = ybegin; ystart < yend; ystart += m_texture_height) {
        for (int xstart = xbegin; xstart < xend; xstart += m_texture_width) {
            int tile_width  = std::min(xend - xstart, m_texture_width);
            int tile_height = std::min(yend - ystart, m_texture_height);
            smax            = tile_width / float(m_texture_width);
            tmax            = tile_height / float(m_texture_height);

            //std::cerr << "xstart: " << xstart << ". ystart: " << ystart << "\n";
            //std::cerr << "tile_width: " << tile_width << ". tile_height: " << tile_height << "\n";

            // FIXME: This can get too slow. Some ideas: avoid sending the tex
            // images more than necessary, figure an optimum texture size, use
            // multiple texture objects.
            load_texture(xstart, ystart, tile_width, tile_height);
            gl_rect(xstart, ystart, xstart + tile_width, ystart + tile_height,
                    0, smin, tmin, smax, tmax);
        }
    }

    if (m_viewer.windowguidesOn()) {
        paint_windowguides();
    }

    if (m_selecting) {
        glPushMatrix();
        glLoadIdentity();

        glPushAttrib(GL_ENABLE_BIT | GL_CURRENT_BIT);
        glDisable(GL_TEXTURE_2D);
        if (m_use_shaders) {
            glUseProgram(0);
        }

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glColor4f(0.2f, 0.5f, 1.0f, 0.3f);  // Light blue fill with transparency

        int w = width();
        int h = height();

        float x1 = m_select_start.x() - w / 2.0f;
        float y1 = -(m_select_start.y() - h / 2.0f);

        float x2 = m_select_end.x() - w / 2.0f;
        float y2 = -(m_select_end.y() - h / 2.0f);

        int left   = std::min(x1, x2);
        int right  = std::max(x1, x2);
        int bottom = std::min(y1, y2);
        int top    = std::max(y1, y2);

        gl_rect(left, bottom, right, top, -0.1f);

        glPopAttrib();
        glPopMatrix();
    }
    glPopMatrix();

    if (m_viewer.pixelviewOn()) {
        paint_pixelview();
    }

    if (m_viewer.probeviewOn()) {
        paint_probeview();
    } else {
        m_area_probe_text.clear();
    }

    // Show the status info again.
    m_viewer.statusProgress->hide();
    m_viewer.statusViewInfo->show();
    unsetCursor();

#ifndef NDEBUG
    std::cerr << "paintGL elapsed time: " << paint_image_time() << " seconds\n";
#endif
}



void
IvGL::shadowed_text(float x, float y, float /*z*/, const std::string& s,
                    const QColor& color)
{
    if (s.empty()) {
        return;
    }

    /*
     * Paint on intermediate QImage, AA text on QOpenGLWidget based
     * QPaintDevice requires MSAA
     */
    qreal dpr = devicePixelRatio();
    QImage t(size() * dpr, QImage::Format_ARGB32_Premultiplied);
    t.setDevicePixelRatio(dpr);
    t.fill(qRgba(0, 0, 0, 0));
    {
        QPainter painter(&t);
        painter.setRenderHint(QPainter::TextAntialiasing, true);
        QFont font;
        font.setFamilies({ "Monaco", "Menlo", "Consolas", "DejaVu Sans Mono",
                           "Courier New" });
        font.setFixedPitch(true);
        font.setPointSize(11);
        painter.setFont(font);
        painter.setPen(QPen(color, 1.0));
        painter.drawText(QPointF(x, y), QString(s.c_str()));
    }
    QPainter painter(this);
    painter.drawImage(rect(), t);
}



static int
num_channels(int current_channel, int nchannels,
             ImageViewer::COLOR_MODE color_mode)
{
    switch (color_mode) {
    case ImageViewer::RGBA: return clamp(nchannels - current_channel, 0, 4);
    case ImageViewer::RGB:
    case ImageViewer::LUMINANCE:
        return clamp(nchannels - current_channel, 0, 3);
        break;
    case ImageViewer::SINGLE_CHANNEL:
    case ImageViewer::HEATMAP: return 1;
    default: return nchannels;
    }
}

// Helper function to calculate min, max, and average for a given type and ROI
template<typename T> struct ChannelStatsResult {
    T min_val, max_val, avg_val;
};

template<typename T>
ChannelStatsResult<T>
calculate_channel_stats(const ImageBuf& img, const ROI& roi, int channel,
                        bool is_inside_data_window)
{
    if (!is_inside_data_window) {
        // Return zeros if not inside data window
        return ChannelStatsResult<T> { T(0), T(0), T(0) };
    }

    int pixel_count = (roi.xend - roi.xbegin) * (roi.yend - roi.ybegin);
    if (pixel_count <= 0) {
        // ROI is empty or invalid, return zeros
        return ChannelStatsResult<T> { T(0), T(0), T(0) };
    }

    T min_val  = std::numeric_limits<T>::max();
    T max_val  = std::numeric_limits<T>::lowest();
    double sum = 0.0;

    ImageBuf::ConstIterator<T, T> it(img, roi);
    for (; !it.done(); ++it) {
        T val   = it[channel];
        min_val = std::min(min_val, val);
        max_val = std::max(max_val, val);
        sum += val;
    }

    ChannelStatsResult<T> result;
    result.min_val = min_val;
    result.max_val = max_val;
    result.avg_val = static_cast<T>(sum / pixel_count);
    return result;
}

void
IvGL::paint_pixelview()
{
    using Strutil::fmt::format;

    IvImage* img = m_current_image;
    const ImageSpec& spec(img->spec());

    // (x_mouse_viewport,y_mouse_viewport) are the window coordinates of the mouse.
    int x_mouse_viewport, y_mouse_viewport;
    get_focus_window_pixel(x_mouse_viewport, y_mouse_viewport);

    // (x_mouse_image,y_mouse_image) are the image-space [0..res-1] position of the mouse.
    int x_mouse_image, y_mouse_image;
    get_focus_image_pixel(x_mouse_image, y_mouse_image);

    glPushMatrix();
    glLoadIdentity();
    // Transform is now same as the main GL viewport -- window pixels as
    // units, with (0,0) at the center of the visible window.

    glTranslatef(0, 0, -1);
    // Pushed away from the camera 1 unit.  This makes the pixel view
    // elements closer to the camera than the main view.

    // n_closeup_pixels is the number of big pixels (in each direction)
    // visible in our closeup window. Guaranteed to be an odd number
    const int n_closeup_pixels = m_viewer.closeupPixels();

    // n_closeup_avg_pixels is the number of pixels used to calculate the average color.
    // it is guaranteed to be no bigger than n_closeup_pixels. Guaranteed to be an odd number
    const int n_closeup_avg_pixels = m_viewer.closeupAvgPixels();

    // number of pixels from the side of the closeup window to the average color window.
    const int avg_window_offset = (n_closeup_pixels - n_closeup_avg_pixels) / 2;

    // closeup_pixel_zoom is the size of single image pixel inside close up window
    const float closeup_pixel_size = static_cast<float>(closeup_window_size)
                                     / n_closeup_pixels;

    // height of a single line of text in the closeup window
    const int text_line_height = 18;

    // number of pixels from the side of the closeup window to the mouse position when it is following the mouse
    const int follow_mouse_offset = 15;

    // total height of all text in the closeup window + padding
    const int total_text_height = (spec.nchannels + 2) * text_line_height + 4;

    // height of the status bar
    const int status_bar_height = 15;  // TODO m_viewer.statusBar()->height();

    // Calculate if closeup would go beyond viewport boundaries
    bool should_show_on_left = (x_mouse_viewport + closeup_window_size
                                + follow_mouse_offset)
                               > width();
    bool should_show_above = (y_mouse_viewport + closeup_window_size
                              + follow_mouse_offset + total_text_height
                              + status_bar_height)
                             > height();

    bool should_follow_mouse = m_viewer.pixelviewFollowsMouse();


    // Use to translate OpenGL coordinate system to render closeup window
    // at the correct position depending on user settings and mouse position
    // OpenGL coordinate system has origin at the bottom left corner of the window
    float x_gl_translate = 0;
    float y_gl_translate = 0;

    if (should_follow_mouse) {
        // Display closeupview next to mouse cursor
        // it is calculated dynamically such that closeup window is always visible
        // even if mouse cursor is close to the edge of main window viewport
        x_gl_translate = x_mouse_viewport - width() / 2
                         + closeup_window_size / 2 + 4 + follow_mouse_offset;
        y_gl_translate = -y_mouse_viewport + height() / 2
                         - closeup_window_size / 2 - 4 - follow_mouse_offset;

        if (should_show_on_left) {
            x_gl_translate -= closeup_window_size + follow_mouse_offset * 2;
        }

        if (should_show_above) {
            y_gl_translate += closeup_window_size + total_text_height
                              + follow_mouse_offset * 2 + 8;
        }

    } else if (m_pixelview_left_corner) {
        // Display closeup in corner -- translate the coordinate system so that
        // it is centered near the corner of the window.
        x_gl_translate = closeup_window_size * 0.5f + 5 - width() / 2;
        y_gl_translate = -closeup_window_size * 0.5f - 5 + height() / 2;

        // If the mouse cursor is over the pixelview closeup when it's on
        // the upper left, switch to the upper right
        if ((x_mouse_viewport < closeup_window_size + 5)
            && y_mouse_viewport < (closeup_window_size + 5 + total_text_height))
            m_pixelview_left_corner = false;

    } else {
        x_gl_translate = -closeup_window_size * 0.5f - 5 + width() / 2;
        y_gl_translate = -closeup_window_size * 0.5f - 5 + height() / 2;

        // If the mouse cursor is over the pixelview closeup when it's on
        // the upper right, switch to the upper left
        if (x_mouse_viewport > (width() - closeup_window_size - 5)
            && y_mouse_viewport < (closeup_window_size + 5 + total_text_height))
            m_pixelview_left_corner = true;
    }

    glTranslatef(x_gl_translate, y_gl_translate, 0);

    // In either case, the GL coordinate system is now scaled to window
    // pixel units, and centered on the middle of where the closeup
    // window is going to appear.  All other coordinates from here on
    // (in this procedure) should be relative to the closeup window center.

    glPushAttrib(GL_ENABLE_BIT | GL_TEXTURE_BIT);
    useshader(closeup_texture_size, closeup_texture_size, true);

    float scale_x  = 1.0f;
    float scale_y  = 1.0f;
    float rotate_z = 0.0f;
    float real_xp  = x_mouse_image;
    float real_yp  = y_mouse_image;
    handle_orientation(img->orientation(), spec.width, spec.height, scale_x,
                       scale_y, rotate_z, real_xp, real_yp, true);

    float smin = 0;
    float tmin = 0;
    float smax = 1.0f;
    float tmax = 1.0f;
    // Calculate patch of the image to use for the pixelview.
    int xbegin = 0;
    int ybegin = 0;
    int xend   = 0;
    int yend   = 0;

    bool is_mouse_inside_image = x_mouse_image >= 0
                                 && x_mouse_image < img->oriented_width()
                                 && y_mouse_image >= 0
                                 && y_mouse_image < img->oriented_height();
    if (is_mouse_inside_image) {
        // Keep the view within n_closeup_pixels pixels.
        int xpp = clamp<int>(real_xp, n_closeup_pixels / 2,
                             spec.width - n_closeup_pixels / 2 - 1);
        int ypp = clamp<int>(real_yp, n_closeup_pixels / 2,
                             spec.height - n_closeup_pixels / 2 - 1);
        xbegin  = std::max(xpp - n_closeup_pixels / 2, 0);
        ybegin  = std::max(ypp - n_closeup_pixels / 2, 0);
        xend    = std::min(xpp + n_closeup_pixels / 2 + 1, spec.width);
        yend    = std::min(ypp + n_closeup_pixels / 2 + 1, spec.height);
        smin    = 0;
        tmin    = 0;
        smax    = float(xend - xbegin) / closeup_texture_size;
        tmax    = float(yend - ybegin) / closeup_texture_size;
        //std::cerr << "img (" << xbegin << "," << ybegin << ") - (" << xend << "," << yend << ")\n";
        //std::cerr << "tex (" << smin << "," << tmin << ") - (" << smax << "," << tmax << ")\n";
        //std::cerr << "center mouse (" << x_mouse_image << "," << y_mouse_image << "), real (" << real_xp << "," << real_yp << ")\n";

        int nchannels = img->nchannels();
        // For simplicity, we don't support more than 4 channels without shaders
        // (yet).
        if (m_use_shaders) {
            nchannels = num_channels(m_viewer.current_channel(), nchannels,
                                     m_viewer.current_color_mode());
        }

        auto zoombuffer = OIIO_ALLOCA_SPAN(std::byte,
                                           (xend - xbegin) * (yend - ybegin)
                                               * nchannels
                                               * spec.channel_bytes());
        if (!m_use_shaders) {
            img->get_pixels(ROI(spec.x + xbegin, spec.x + xend, spec.y + ybegin,
                                spec.y + yend),
                            spec.format, zoombuffer);
        } else {
            ROI roi(spec.x + xbegin, spec.x + xend, spec.y + ybegin,
                    spec.y + yend, 0, 1, m_viewer.current_channel(),
                    m_viewer.current_channel() + nchannels);
            img->get_pixels(roi, spec.format, zoombuffer);
        }

        GLenum glformat, gltype, glinternalformat;
        typespec_to_opengl(spec, nchannels, gltype, glformat, glinternalformat);
        // Use pixelview's own texture, and upload the corresponding image patch.
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        glBindTexture(GL_TEXTURE_2D, m_pixelview_tex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, xend - xbegin, yend - ybegin,
                        glformat, gltype, zoombuffer.data());
        print_error("After tsi2d");
    } else {
        smin = -1;
        smax = -1;
        glDisable(GL_TEXTURE_2D);
        glColor3f(0.1f, 0.1f, 0.1f);
    }
    if (!m_use_shaders) {
        glDisable(GL_BLEND);
    }

    glPushMatrix();
    glScalef(1, -1, 1);  // Run y from top to bottom.
    glScalef(scale_x, scale_y, 1);
    glRotatef(rotate_z, 0, 0, 1);

    // This square is the closeup window itself
    gl_rect(-0.5f * closeup_window_size, -0.5f * closeup_window_size,
            0.5f * closeup_window_size, 0.5f * closeup_window_size, 0, smin,
            tmin, smax, tmax);
    glPopMatrix();
    glPopAttrib();

    // Draw a second window, slightly behind the closeup window, as a
    // backdrop.  It's partially transparent, having the effect of
    // darkening the main image view beneath the closeup window.  It
    // extends slightly out from the closeup window (making it more
    // clearly visible), and also all the way down to cover the area
    // where the text will be printed, so it is very readable.


    glPushAttrib(GL_ENABLE_BIT | GL_CURRENT_BIT);
    glDisable(GL_TEXTURE_2D);
    if (m_use_shaders) {
        // Disable shaders for this.
        glUseProgram(0);
    }

    glColor4f(0.1f, 0.1f, 0.1f, 0.7f);
    gl_rect(-0.5f * closeup_window_size, -0.5f * closeup_window_size,
            0.5f * closeup_window_size,
            -0.5f * closeup_window_size - total_text_height, -0.1f);

    // Colors for text and corner indicator of center pixel (Val) being measured
    QColor center_pix_value_color(0, 255, 255, 125);
    // Color for text and corner indicator of all pixels used for calculating average value
    QColor avg_value_color(255, 255, 0, 125);

    int pixel_x = (int)real_xp + spec.x;
    int pixel_y = (int)real_yp + spec.y;

    // array of channel values for pixel under mouse cursor
    float* fpixel = OIIO_ALLOCA(float, spec.nchannels);
    img->getpixel(pixel_x, pixel_y, fpixel);

    // String values to be printed in stats table for each channel
    struct ChannelStats {
        std::string name;
        std::string centerValue;
        std::string normalized;
        std::string min;
        std::string max;
        std::string avg;
    };
    std::vector<ChannelStats> channels_stats;

    // Maximum length of each string value in the stats table among all channels
    struct MaxLengths {
        int name        = 0;
        int centerValue = 0;
        int normalized  = 0;
        int min         = 0;
        int max         = 0;
        int avg         = 0;
    };
    MaxLengths maxLengths;

    const int MAX_NAME_LENGTH = 10;

    bool is_inside_data_window = ybegin > 0 || yend > 0 || xbegin > 0
                                 || xend > 0;

    // Calculate the ROI for min/max/avg calculation
    ROI avg_roi(spec.x + xbegin + avg_window_offset,
                spec.x + xend - avg_window_offset,
                spec.y + ybegin + avg_window_offset,
                spec.y + yend - avg_window_offset);

    // Lambda for smart float formatting (max 5 chars including decimal)
    auto format_float = [](float value) -> std::string {
        if (value < 10) {
            return format("{:.3f}", value);
        } else if (value < 100) {
            return format("{:.2f}", value);
        } else if (value < 1000) {
            return format("{:.1f}", value);
        } else {
            return format("{:.0f}", value);
        }
    };

    for (int channel = 0; channel < spec.nchannels; ++channel) {
        std::string name = spec.channelnames[channel];
        // Truncate channel name if longer than 10 characters
        if (name.length() > MAX_NAME_LENGTH) {
            name = format("{:.4}...{}", name, name.substr(name.length() - 3));
        }
        std::string centerValue;
        std::string normalized;
        std::string min;
        std::string max;
        std::string avg;

        /* 
        For each channel we calculate:
        - center value (value of pixel under mouse cursor)
        - normalized value (value of pixel under mouse cursor divided by max value of all pixels in the closeup window)
        - min, max and average value of all pixels in the averaging subset of pixels of closeup window

        There are three almost identical cases for different pixel types.
        */

        switch (spec.format.basetype) {
        case TypeDesc::UINT8: {
            ImageBuf::ConstIterator<unsigned char, unsigned char> p(*img,
                                                                    pixel_x,
                                                                    pixel_y);
            std::string center_value_separation_spaces(5, ' ');
            auto stats
                = calculate_channel_stats<unsigned char>(*img, avg_roi, channel,
                                                         is_inside_data_window);

            centerValue = format("{:<3}", int(p[channel]));
            normalized  = format("{:3.3f}", fpixel[channel])
                         + center_value_separation_spaces;
            min = format("{:<3}", stats.min_val);
            max = format("{:<3}", stats.max_val);
            avg = format("{:<3}", stats.avg_val);
            break;
        }
        case TypeDesc::UINT16: {
            ImageBuf::ConstIterator<unsigned short, unsigned short> p(*img,
                                                                      pixel_x,
                                                                      pixel_y);
            std::string center_value_separation_spaces(2, ' ');
            auto stats = calculate_channel_stats<unsigned short>(
                *img, avg_roi, channel, is_inside_data_window);

            centerValue = format("{:<5}", int(p[channel]));
            normalized  = format("{:3.3f}", fpixel[channel])
                         + center_value_separation_spaces;
            min = format("{:<5}", stats.min_val);
            max = format("{:<5}", stats.max_val);
            avg = format("{:<5}", stats.avg_val);
            break;
        }
        case TypeDesc::HALF: {
            auto stats = calculate_channel_stats<half>(*img, avg_roi, channel,
                                                       is_inside_data_window);

            centerValue = format_float(fpixel[channel]);
            normalized  = "";  // No normalized value for float
            min         = format_float(stats.min_val);
            max         = format_float(stats.max_val);
            avg         = format_float(stats.avg_val);
            break;
        }
        default: {  // everything else, treat as float
            auto stats = calculate_channel_stats<float>(*img, avg_roi, channel,
                                                        is_inside_data_window);

            centerValue = format_float(fpixel[channel]);
            normalized  = "";  // No normalized value for float
            min         = format_float(stats.min_val);
            max         = format_float(stats.max_val);
            avg         = format_float(stats.avg_val);
            break;
        }
        }

        maxLengths.name        = std::max(maxLengths.name, (int)name.length());
        maxLengths.centerValue = std::max(maxLengths.centerValue,
                                          (int)centerValue.length());
        maxLengths.normalized  = std::max(maxLengths.normalized,
                                          (int)normalized.length());
        maxLengths.min         = std::max(maxLengths.min, (int)min.length());
        maxLengths.max         = std::max(maxLengths.max, (int)max.length());
        maxLengths.avg         = std::max(maxLengths.avg, (int)avg.length());

        channels_stats.push_back(
            { name, centerValue, normalized, min, max, avg });
    }


    // Now we print text giving the mouse coordinates and the numerical
    // values of the pixel that the mouse is over.
    int x_text, y_text;
    if (should_follow_mouse) {
        x_text = x_mouse_viewport + 8 + follow_mouse_offset;
        y_text = y_mouse_viewport + closeup_window_size + text_line_height
                 + follow_mouse_offset;

        if (should_show_on_left) {
            x_text -= closeup_window_size + follow_mouse_offset * 2;
        }

        if (should_show_above) {
            y_text -= closeup_window_size + total_text_height
                      + follow_mouse_offset * 2 + 8;
        }
    } else if (m_pixelview_left_corner) {
        x_text = 9;
        y_text = closeup_window_size + text_line_height;
    } else {
        x_text = width() - closeup_window_size - 1;
        y_text = closeup_window_size + text_line_height;
    }

    QColor normal_text_color(200, 200, 200);
    // Extra spaces to be added after value in case of float values. Depends on the length of the channel name.
    std::string float_spaces_post_value_str(MAX_NAME_LENGTH - maxLengths.name,
                                            ' ');

    {
        QColor center_pix_value_text_color = center_pix_value_color;
        center_pix_value_text_color.setAlpha(200);

        QColor avg_value_text_color = avg_value_color;
        avg_value_text_color.setAlpha(200);

        std::string mouse_pos = format("              ({:d},{:d})",
                                       (int)real_xp, (int)real_yp);
        shadowed_text(x_text, y_text, 0.0f, mouse_pos,
                      center_pix_value_text_color);
        y_text += text_line_height;

        // TODO Find a nicer way of doing this.
        // Next three blocks are a hacky way of rendering a table header with
        // Val, Norm and Min, Max, Avg rendered in a different color from rest of the text.
        // It is done by rendering three texts on top of another.

        // Build the "Norm" column header conditionally
        std::string normalized_header;
        std::string empty_normalized_header;
        if (maxLengths.normalized > 0) {
            normalized_header       = format("{:<{}}  ", "Norm",
                                             maxLengths.normalized);
            empty_normalized_header = format("{:<{}}  ", "    ",
                                             maxLengths.normalized);
        } else {
            normalized_header       = float_spaces_post_value_str;
            empty_normalized_header = float_spaces_post_value_str;
        }

        // Print "Val" column headers with normal cyan color
        std::string val_header
            = format("{:<{}}  {:<{}}  {}{:<{}}  {:<{}}  {:<{}}  ", " ",
                     maxLengths.name, "Val", maxLengths.centerValue,
                     normalized_header, "   ", maxLengths.min, "   ",
                     maxLengths.max, "   ", maxLengths.avg);
        shadowed_text(x_text, y_text, 0.0f, val_header,
                      center_pix_value_text_color);

        // Print "Avg" column header with normal yellow color
        std::string avg_header
            = format("{:<{}}  {:<{}}  {}{:<{}}  {:<{}}  {:<{}}  ", " ",
                     maxLengths.name, "   ", maxLengths.centerValue,
                     empty_normalized_header, "Min", maxLengths.min, "Max",
                     maxLengths.max, "Avg", maxLengths.avg);
        shadowed_text(x_text, y_text, 0.0f, avg_header, avg_value_text_color);

        y_text += text_line_height;
    }

    for (const auto& stat : channels_stats) {
        // Build the "Norm" column header conditionally
        std::string normalized_col;
        if (maxLengths.normalized > 0) {
            normalized_col = format("{:<{}}  ", stat.normalized,
                                    maxLengths.normalized);
        } else {
            normalized_col = float_spaces_post_value_str;
        }

        std::string line = format("{:<{}}: {:<{}}  {}{:<{}}  {:<{}}  {:<{}}  ",
                                  stat.name, maxLengths.name, stat.centerValue,
                                  maxLengths.centerValue, normalized_col,
                                  stat.min, maxLengths.min, stat.max,
                                  maxLengths.max, stat.avg, maxLengths.avg);

        QColor channelColor;
        if (stat.name[0] == 'R') {
            channelColor = QColor(250, 94, 143);
        } else if (stat.name[0] == 'G') {
            channelColor = QColor(135, 203, 124);
        } else if (stat.name[0] == 'B') {
            channelColor = QColor(107, 188, 255);
        } else {
            channelColor = normal_text_color;
        }

        shadowed_text(x_text, y_text, 0.0f, line, channelColor);
        y_text += text_line_height;
    }

    glPopAttrib();
    glPopMatrix();

    // Draw cyan corners around center pixel
    if (is_mouse_inside_image) {
        // Draw corner markers
        auto draw_corners = [](QPainter& painter, float rect_x1, float rect_y1,
                               float rect_x2, float rect_y2,
                               const QColor& color) {
            float corner_size = 4;  // Size of each corner marker
            painter.setPen(QPen(color, 1.0));

            // Top-left corner
            painter.drawLine(rect_x1, rect_y1, rect_x1 + corner_size, rect_y1);
            painter.drawLine(rect_x1, rect_y1, rect_x1, rect_y1 + corner_size);

            // Top-right corner
            painter.drawLine(rect_x2 - corner_size, rect_y1, rect_x2, rect_y1);
            painter.drawLine(rect_x2, rect_y1, rect_x2, rect_y1 + corner_size);

            // Bottom-left corner
            painter.drawLine(rect_x1, rect_y2 - corner_size, rect_x1, rect_y2);
            painter.drawLine(rect_x1, rect_y2, rect_x1 + corner_size, rect_y2);

            // Bottom-right corner
            painter.drawLine(rect_x2 - corner_size, rect_y2, rect_x2, rect_y2);
            painter.drawLine(rect_x2, rect_y2 - corner_size, rect_x2, rect_y2);
        };

        // Size of each pixel in the view taking into account spacing between pixels
        float pixel_size = closeup_pixel_size - 1;
        // Top left corner for the rect around center pixel
        float rect_x1;  // Left edge
        float rect_y1;  // Top edge

        float offset_from_closeup_window = closeup_window_size / 2
                                           - pixel_size / 2 + 5;
        if (should_follow_mouse) {
            rect_x1 = x_mouse_viewport + offset_from_closeup_window
                      + follow_mouse_offset;
            rect_y1 = y_mouse_viewport + offset_from_closeup_window
                      + follow_mouse_offset;

            if (should_show_on_left) {
                rect_x1 -= closeup_window_size + follow_mouse_offset * 2;
            }

            if (should_show_above) {
                rect_y1 -= closeup_window_size + total_text_height
                           + follow_mouse_offset * 2 + 8;
            }
        } else if (m_pixelview_left_corner) {
            rect_x1 = offset_from_closeup_window + 1;
            rect_y1 = offset_from_closeup_window + 1;
        } else {
            rect_x1 = width() - offset_from_closeup_window - pixel_size;
            rect_y1 = offset_from_closeup_window + 1;
        }

        QPainter painter(this);
        if (avg_window_offset > 0) {
            // Drawing indicators of avg sub-section of the closeup window
            // before adjusting center pixel position because avg is not shifted to the edges
            short int center_to_avg_window_offset = n_closeup_pixels / 2
                                                    - avg_window_offset;
            float avg_x1 = rect_x1
                           - center_to_avg_window_offset * closeup_pixel_size;
            float avg_y1 = rect_y1
                           - center_to_avg_window_offset * closeup_pixel_size;
            float avg_x2 = rect_x1
                           + (center_to_avg_window_offset + 1)
                                 * closeup_pixel_size;
            float avg_y2 = rect_y1
                           + (center_to_avg_window_offset + 1)
                                 * closeup_pixel_size;
            draw_corners(painter, avg_x1, avg_y1, avg_x2, avg_y2,
                         avg_value_color);
        }

        // Adjust x and y of measured pixel position to account for the fact that the
        // center pixel is not at the center of the closeup window
        // in situation when pixel mouse is hovering over is close to the edge of an image
        float half_closeup_window_size = n_closeup_pixels / 2;
        short int px_to_right_edge     = spec.width - pixel_x;
        short int px_to_bottom_edge    = spec.height - pixel_y;

        bool is_close_to_right_edge = px_to_right_edge
                                      <= half_closeup_window_size;
        bool is_close_to_bottom_edge = px_to_bottom_edge
                                       <= half_closeup_window_size;
        bool is_close_to_left_edge = pixel_x <= half_closeup_window_size;
        bool is_close_to_top_edge  = pixel_y <= half_closeup_window_size;

        if (is_close_to_right_edge) {
            rect_x1 += +(half_closeup_window_size - px_to_right_edge + 1)
                           * closeup_pixel_size
                       + 1;
        }

        if (is_close_to_bottom_edge) {
            rect_y1 += +(half_closeup_window_size - px_to_bottom_edge + 1)
                           * closeup_pixel_size
                       + 1;
        }

        if (is_close_to_left_edge) {
            rect_x1 -= (half_closeup_window_size - pixel_x) * closeup_pixel_size
                       + 1;
        }
        if (is_close_to_top_edge) {
            rect_y1 -= (half_closeup_window_size - pixel_y) * closeup_pixel_size
                       + 1;
        }

        float rect_x2 = rect_x1 + pixel_size;  // Right edge
        float rect_y2 = rect_y1 + pixel_size;  // Bottom edge

        draw_corners(painter, rect_x1, rect_y1, rect_x2, rect_y2,
                     center_pix_value_color);
    }
}



void
IvGL::paint_probeview()
{
    if (!m_current_image)
        return;
    IvImage* img = m_current_image;
    const ImageSpec& spec(img->spec());

    int x_mouse_viewport, y_mouse_viewport;
    get_focus_window_pixel(x_mouse_viewport, y_mouse_viewport);

    glPushMatrix();
    glLoadIdentity();

    // Set to window pixel units and center the origin
    glTranslatef(0, 0, -1);  // Push into screen to draw on top

    float closeup_width  = closeup_window_size * 1.3f;
    float closeup_height = closeup_window_size * (0.06f * (spec.nchannels + 1));

    // Position the close-up box
    const float status_bar_offset = 35.0f;
    glTranslatef(closeup_width * 0.5f + 5 - width() / 2,
                 closeup_height * 0.5f + status_bar_offset - height() / 2, 0);

    glPushAttrib(GL_ENABLE_BIT | GL_CURRENT_BIT);
    glDisable(GL_TEXTURE_2D);
    if (m_use_shaders)
        glUseProgram(0);
    float extraspace = 10 * (1 + spec.nchannels) + 4;
    glColor4f(0.1f, 0.1f, 0.1f, 0.5f);
    gl_rect(-0.5f * closeup_width - 2, 0.5f * closeup_height + 10 + 2,
            0.5f * closeup_width + 2, -0.5f * closeup_height - extraspace,
            -0.1f);
    // Draw probe text
    QFont font;

    int x_text   = 9;
    int y_text   = height() - closeup_height - 30;
    int yspacing = 15;

    if (m_area_probe_text.empty()) {
        std::ostringstream oss;  // Output stream
        oss << "Area Probe:\n";
        for (int i = 0; i < spec.nchannels; ++i)
            oss << spec.channel_name(i)
                << ":   [min:  -----, max:  -----, avg:  -----]\n";
        m_area_probe_text = oss.str();
    }

    std::istringstream iss(m_area_probe_text);
    std::string line;
    while (std::getline(iss, line)) {
        shadowed_text(x_text, y_text, 0.0f, line);
        y_text += yspacing;
    }

    glPopAttrib();
    glPopMatrix();
}



void
IvGL::paint_windowguides()
{
    IvImage* img = m_current_image;
    const ImageSpec& spec(img->spec());

    glDisable(GL_TEXTURE_2D);
    glUseProgram(0);
    glPushAttrib(GL_ENABLE_BIT);
    glEnable(GL_COLOR_LOGIC_OP);
    glLogicOp(GL_XOR);

    // Data window
    {
        const float xmin = spec.x;
        const float xmax = spec.x + spec.width;
        const float ymin = spec.y;
        const float ymax = spec.y + spec.height;
        gl_rect_border(xmin, ymin, xmax, ymax);
    }

    // Display window
    {
        const float xmin = spec.full_x;
        const float xmax = spec.full_x + spec.full_width;
        const float ymin = spec.full_y;
        const float ymax = spec.full_y + spec.full_height;
        gl_rect_dotted_border(xmin, ymin, xmax, ymax);
    }

    glPopAttrib();
}



void
IvGL::useshader(int tex_width, int tex_height, bool pixelview)
{
    if (!m_use_shaders) {
        glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
        for (auto&& tb : m_texbufs) {
            glBindTexture(GL_TEXTURE_2D, tb.tex_object);
            if (m_viewer.linearInterpolation()) {
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                                GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                                GL_LINEAR);
            } else {
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                                GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                                GL_NEAREST);
            }
        }
        return;
    }

    use_program();
    update_uniforms(tex_width, tex_height, pixelview);
}



void
IvGL::use_program(void)
{
    glUseProgram(m_shader_program);
    print_error("After use program");
}



void
IvGL::update_uniforms(int tex_width, int tex_height, bool pixelview)
{
    IvImage* img = m_viewer.cur();
    if (!img)
        return;

    const ImageSpec& spec(img->spec());

    GLint loc;

    loc = glGetUniformLocation(m_shader_program, "startchannel");
    if (m_viewer.current_channel() >= spec.nchannels) {
        glUniform1i(loc, -1);
        return;
    }
    glUniform1i(loc, 0);

    loc = glGetUniformLocation(m_shader_program, "imgtex");
    // This is the texture unit, not the texture object
    glUniform1i(loc, 0);

    loc        = glGetUniformLocation(m_shader_program, "gain");
    float gain = powf(2.0, img->exposure());
    glUniform1f(loc, gain);

    loc = glGetUniformLocation(m_shader_program, "gamma");
    glUniform1f(loc, img->gamma());

    loc = glGetUniformLocation(m_shader_program, "colormode");
    glUniform1i(loc, m_viewer.current_color_mode());

    loc = glGetUniformLocation(m_shader_program, "imgchannels");
    glUniform1i(loc, spec.nchannels);

    loc = glGetUniformLocation(m_shader_program, "pixelview");
    glUniform1i(loc, pixelview);

    loc = glGetUniformLocation(m_shader_program, "linearinterp");
    glUniform1i(loc, m_viewer.linearInterpolation());

    loc = glGetUniformLocation(m_shader_program, "width");
    glUniform1i(loc, tex_width);

    loc = glGetUniformLocation(m_shader_program, "height");
    glUniform1i(loc, tex_height);

    print_error("After setting uniforms");
}



void
IvGL::update()
{
    //std::cerr << "update image\n";

    IvImage* img = m_viewer.cur();
    if (!img) {
        m_current_image = NULL;
        return;
    }

    const ImageSpec& spec(img->spec());

    int nchannels = img->nchannels();
    // For simplicity, we don't support more than 4 channels without shaders
    // (yet).
    if (m_use_shaders) {
        nchannels = num_channels(m_viewer.current_channel(), nchannels,
                                 m_viewer.current_color_mode());
    }

    if (!nchannels)
        return;  // Don't bother, the shader will show blackness for us.

    GLenum gltype           = GL_UNSIGNED_BYTE;
    GLenum glformat         = GL_RGB;
    GLenum glinternalformat = GL_RGB;
    typespec_to_opengl(spec, nchannels, gltype, glformat, glinternalformat);

    m_texture_width  = clamp(ceil2(spec.width), 1, m_max_texture_size);
    m_texture_height = clamp(ceil2(spec.height), 1, m_max_texture_size);

    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

    for (auto&& tb : m_texbufs) {
        tb.width  = 0;
        tb.height = 0;
        glBindTexture(GL_TEXTURE_2D, tb.tex_object);
        glTexImage2D(GL_TEXTURE_2D, 0 /*mip level*/, glinternalformat,
                     m_texture_width, m_texture_height, 0 /*border width*/,
                     glformat, gltype, NULL /*data*/);
        print_error("Setting up texture");
    }

    // Set the right type for the texture used for pixelview.
    glBindTexture(GL_TEXTURE_2D, m_pixelview_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, glinternalformat, closeup_texture_size,
                 closeup_texture_size, 0, glformat, gltype, NULL);
    print_error("Setting up pixelview texture");

    // Resize the buffer at once, rather than create one each drawing.
    m_tex_buffer.resize(m_texture_width * m_texture_height * nchannels
                        * spec.channel_bytes());
    m_current_image = img;
}



void
IvGL::view(float xcenter, float ycenter, float zoom, bool redraw)
{
    m_centerx = xcenter;
    m_centery = ycenter;
    m_zoom    = zoom;

    if (redraw)
        parent_t::update();
}



void
IvGL::zoom(float newzoom, bool redraw)
{
    view(m_centerx, m_centery, newzoom, redraw);
}



void
IvGL::center(float x, float y, bool redraw)
{
    view(x, y, m_viewer.zoom(), redraw);
}



void
IvGL::pan(float dx, float dy)
{
    center(m_centerx + dx, m_centery + dy);
}



void
IvGL::remember_mouse(const QPoint& pos)
{
    m_mousex = pos.x();
    m_mousey = pos.y();
}



void
IvGL::clamp_view_to_window()
{
    IvImage* img = m_current_image;
    if (!img)
        return;
    int w = width(), h = height();
    float zoomedwidth  = m_zoom * img->oriented_full_width();
    float zoomedheight = m_zoom * img->oriented_full_height();
#if 0
    float left    = m_centerx - 0.5 * ((float)w / m_zoom);
    float top     = m_centery - 0.5 * ((float)h / m_zoom);
    float right   = m_centerx + 0.5 * ((float)w / m_zoom);
    float bottom  = m_centery + 0.5 * ((float)h / m_zoom);
    std::cerr << "Window size is " << w << " x " << h << "\n";
    std::cerr << "Center (pixel coords) is " << m_centerx << ", " << m_centery << "\n";
    std::cerr << "Top left (pixel coords) is " << left << ", " << top << "\n";
    std::cerr << "Bottom right (pixel coords) is " << right << ", " << bottom << "\n";
#endif

    int xmin = std::min(img->oriented_x(), img->oriented_full_x());
    int xmax = std::max(img->oriented_x() + img->oriented_width(),
                        img->oriented_full_x() + img->oriented_full_width());
    int ymin = std::min(img->oriented_y(), img->oriented_full_y());
    int ymax = std::max(img->oriented_y() + img->oriented_height(),
                        img->oriented_full_y() + img->oriented_full_height());

    // Don't let us scroll off the edges
    if (zoomedwidth >= w) {
        m_centerx = OIIO::clamp(m_centerx, xmin + 0.5f * w / m_zoom,
                                xmax - 0.5f * w / m_zoom);
    } else {
        m_centerx = img->oriented_full_x() + img->oriented_full_width() / 2;
    }

    if (zoomedheight >= h) {
        m_centery = OIIO::clamp(m_centery, ymin + 0.5f * h / m_zoom,
                                ymax - 0.5f * h / m_zoom);
    } else {
        m_centery = img->oriented_full_y() + img->oriented_full_height() / 2;
    }
}



void
IvGL::update_area_probe_text()
{
    IvImage* img = m_current_image;
    const ImageSpec& spec(img->spec());
    // (x_mouse_viewport,y_mouse_viewport) are the window coordinates of the mouse.
    int x_mouse_viewport, y_mouse_viewport;
    get_focus_window_pixel(x_mouse_viewport, y_mouse_viewport);

    int x1, y1;
    get_given_image_pixel(x1, y1, m_select_start.x(), m_select_start.y());

    int x2, y2;
    get_given_image_pixel(x2, y2, m_select_end.x(), m_select_end.y());

    float scale_x  = 1.0f;
    float scale_y  = 1.0f;
    float rotate_z = 0.0f;
    float x1_img   = x1;
    float y1_img   = y1;
    float x2_img   = x2;
    float y2_img   = y2;

    handle_orientation(img->orientation(), spec.width, spec.height, scale_x,
                       scale_y, rotate_z, x1_img, y1_img, true);
    handle_orientation(img->orientation(), spec.width, spec.height, scale_x,
                       scale_y, rotate_z, x2_img, y2_img, true);

    x1_img = clamp<int>(x1_img, 0, spec.width - 1);
    x2_img = clamp<int>(x2_img, 0, spec.width - 1);
    y1_img = clamp<int>(y1_img, 0, spec.height - 1);
    y2_img = clamp<int>(y2_img, 0, spec.height - 1);

    int xmin = std::min(x1_img, x2_img);
    int xmax = std::max(x1_img, x2_img);
    int ymin = std::min(y1_img, y2_img);
    int ymax = std::max(y1_img, y2_img);

    // Min and max
    std::vector<float> min_vals(spec.nchannels,
                                std::numeric_limits<float>::max());
    std::vector<float> max_vals(spec.nchannels,
                                std::numeric_limits<float>::lowest());
    std::vector<double> sums(spec.nchannels, 0.0);
    int count = 0;

    // loop through each pixel
    float* fpixel = OIIO_ALLOCA(float, spec.nchannels);
    for (int y = ymin; y <= ymax; ++y) {
        for (int x = xmin; x <= xmax; ++x) {
            img->getpixel(x + spec.x, y + spec.y, fpixel);
            for (int c = 0; c < spec.nchannels; ++c) {
                min_vals[c] = std::min(min_vals[c], fpixel[c]);
                max_vals[c] = std::max(max_vals[c], fpixel[c]);
                sums[c] += fpixel[c];
            }
            ++count;
        }
    }

    QString result = "Area Probe:\n";
    for (int c = 0; c < spec.nchannels; ++c) {
        float avg = (count > 0) ? static_cast<float>(sums[c] / count) : 0.0f;
        result += QString("%1: [min: %2  max: %3  avg: %4]\n")
                      .arg(QString::fromStdString(spec.channel_name(c))
                               .leftJustified(5))
                      .arg(min_vals[c], 6, 'f', 3)
                      .arg(max_vals[c], 6, 'f', 3)
                      .arg(avg, 6, 'f', 3);
    }

    m_area_probe_text = result.toStdString();
}



void
IvGL::mousePressEvent(QMouseEvent* event)
{
    remember_mouse(event->pos());
    int mousemode = m_viewer.mouseModeComboBox->currentIndex();
    bool areaMode = m_viewer.areaSampleMode();
    bool Alt      = (event->modifiers() & Qt::AltModifier);
    m_drag_button = event->button();
    if (!m_mouse_activation) {
        switch (event->button()) {
        case Qt::LeftButton:
            if (areaMode) {
                m_select_start = event->pos();
                m_select_end   = m_select_start;
                m_selecting    = true;
                parent_t::update();
            } else if (mousemode == ImageViewer::MouseModeSelect && !Alt
                       && areaMode) {
                std::cerr << areaMode;
                m_select_start = event->pos();
                m_select_end   = m_select_start;
                m_selecting    = true;
                parent_t::update();
            } else if (mousemode == ImageViewer::MouseModeZoom && !Alt
                       && !areaMode) {
                m_viewer.zoomIn(true);  // Animated zoom for mouse clicks
            } else
                m_dragging = true;
            return;
        case Qt::RightButton:
            if (mousemode == ImageViewer::MouseModeZoom && !Alt && !areaMode)
                m_viewer.zoomOut(true);  // Animated zoom for mouse clicks
            else
                m_dragging = true;
            return;
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
        case Qt::MiddleButton:
#else
        case Qt::MidButton:
#endif
            m_dragging = true;
            // FIXME: should this be return rather than break?
            break;
        default: break;
        }
    } else
        m_mouse_activation = false;
    parent_t::mousePressEvent(event);
}



void
IvGL::mouseReleaseEvent(QMouseEvent* event)
{
    remember_mouse(event->pos());
    m_drag_button = Qt::NoButton;
    m_dragging    = false;
    if (m_selecting) {
        m_select_end = event->pos();
        m_selecting  = false;
        update_area_probe_text();
        m_select_start = QPoint();
        m_select_end   = QPoint();
        parent_t::update();
    }
    parent_t::mouseReleaseEvent(event);
}



void
IvGL::mouseMoveEvent(QMouseEvent* event)
{
    QPoint pos = event->pos();

    // Area probe override
    if (m_viewer.areaSampleMode() && m_selecting) {
        m_select_end = event->pos();
        update_area_probe_text();
        remember_mouse(pos);
        parent_t::update();
        if (m_viewer.pixelviewOn()) {
            parent_t::update();
        }
        parent_t::mouseMoveEvent(event);
        return;
    }

    // FIXME - there's probably a better Qt way than tracking the button
    // myself.
    bool Alt      = (event->modifiers() & Qt::AltModifier);
    int mousemode = m_viewer.mouseModeComboBox->currentIndex();
    bool do_pan = false, do_zoom = false, do_wipe = false;
    bool do_select = false, do_annotate = false;
    switch (mousemode) {
    case ImageViewer::MouseModeZoom:
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
        if ((m_drag_button == Qt::MiddleButton)
#else
        if ((m_drag_button == Qt::MidButton)
#endif
            || (m_drag_button == Qt::LeftButton && Alt)) {
            do_pan = true;
        } else if (m_drag_button == Qt::RightButton && Alt) {
            do_zoom = true;
        }
        break;
    case ImageViewer::MouseModePan:
        if (m_drag_button != Qt::NoButton)
            do_pan = true;
        break;
    case ImageViewer::MouseModeWipe:
        if (m_drag_button != Qt::NoButton)
            do_wipe = true;
        break;
    case ImageViewer::MouseModeSelect:
        if (m_drag_button != Qt::NoButton)
            do_select = true;
        break;
    case ImageViewer::MouseModeAnnotate:
        if (m_drag_button != Qt::NoButton)
            do_annotate = true;
        break;
    }
    if (do_pan) {
        float dx = (pos.x() - m_mousex) / m_zoom;
        float dy = (pos.y() - m_mousey) / m_zoom;
        pan(-dx, -dy);
    } else if (do_zoom) {
        float dx = (pos.x() - m_mousex);
        float dy = (pos.y() - m_mousey);
        float z  = m_viewer.zoom() * (1.0 + 0.005 * (dx + dy));
        z        = OIIO::clamp(z, 0.01f, 256.0f);
        m_viewer.zoom(z);
        m_viewer.fitImageToWindowAct->setChecked(false);
    } else if (do_wipe) {
        // FIXME -- unimplemented
    } else if (do_select) {
        if (m_selecting) {
            m_select_end = event->pos();
            parent_t::update();
        }
        // FIXME -- unimplemented
    } else if (do_annotate) {
        // FIXME -- unimplemented
    }
    remember_mouse(pos);
    if (m_viewer.pixelviewOn())
        parent_t::update();
    parent_t::mouseMoveEvent(event);
}



void
IvGL::wheelEvent(QWheelEvent* event)
{
    m_mouse_activation = false;
    QPoint angdelta    = event->angleDelta() / 8;  // div by 8 to get degrees
    if (abs(angdelta.y()) > abs(angdelta.x())      // predominantly vertical
        && abs(angdelta.y()) > 2) {                // suppress tiny motions
        if (angdelta.y() > 0) {
            m_viewer.zoomIn(false);
        } else {
            m_viewer.zoomOut(false);
        }
        event->accept();
    }
    // TODO: Update this to keep the zoom centered on the event .x, .y
}



void
IvGL::focusOutEvent(QFocusEvent*)
{
    m_mouse_activation = true;
}



void
IvGL::get_focus_window_pixel(int& x, int& y)
{
    x = m_mousex;
    y = m_mousey;
}



void
IvGL::get_given_image_pixel(int& x, int& y, int mouseX, int mouseY)
{
    int w = width(), h = height();
    float z = m_zoom;
    // left,top,right,bottom are the borders of the visible window, in
    // pixel coordinates
    float left   = m_centerx - 0.5 * w / z;
    float top    = m_centery - 0.5 * h / z;
    float right  = m_centerx + 0.5 * w / z;
    float bottom = m_centery + 0.5 * h / z;
    // normx,normy are the position of the mouse, in normalized (i.e. [0..1])
    // visible window coordinates.
    float normx = (float)(mouseX + 0.5f) / w;
    float normy = (float)(mouseY + 0.5f) / h;
    // imgx,imgy are the position of the mouse, in pixel coordinates
    float imgx = OIIO::lerp(left, right, normx);
    float imgy = OIIO::lerp(top, bottom, normy);
    // So finally x,y are the coordinates of the image pixel (on [0,res-1])
    // underneath the mouse cursor.
    //FIXME: Shouldn't this take image rotation into account?
    x = (int)floorf(imgx);
    y = (int)floorf(imgy);
}



void
IvGL::get_focus_image_pixel(int& x, int& y)
{
    // w,h are the dimensions of the visible window, in pixels
    int w = width(), h = height();
    float z = m_zoom;
    // left,top,right,bottom are the borders of the visible window, in
    // pixel coordinates
    float left   = m_centerx - 0.5 * w / z;
    float top    = m_centery - 0.5 * h / z;
    float right  = m_centerx + 0.5 * w / z;
    float bottom = m_centery + 0.5 * h / z;
    // normx,normy are the position of the mouse, in normalized (i.e. [0..1])
    // visible window coordinates.
    float normx = (float)(m_mousex + 0.5f) / w;
    float normy = (float)(m_mousey + 0.5f) / h;
    // imgx,imgy are the position of the mouse, in pixel coordinates
    float imgx = OIIO::lerp(left, right, normx);
    float imgy = OIIO::lerp(top, bottom, normy);
    // So finally x,y are the coordinates of the image pixel (on [0,res-1])
    // underneath the mouse cursor.
    //FIXME: Shouldn't this take image rotation into account?
    x = (int)floorf(imgx);
    y = (int)floorf(imgy);

#if 0
    std::cerr << "get_focus_pixel\n";
    std::cerr << "    mouse window pixel coords " << m_mousex << ' ' << m_mousey << "\n";
    std::cerr << "    mouse window NDC coords " << normx << ' ' << normy << '\n';
    std::cerr << "    center image coords " << m_centerx << ' ' << m_centery << "\n";
    std::cerr << "    left,top = " << left << ' ' << top << "\n";
    std::cerr << "    right,bottom = " << right << ' ' << bottom << "\n";
    std::cerr << "    mouse image coords " << imgx << ' ' << imgy << "\n";
    std::cerr << "    mouse pixel image coords " << x << ' ' << y << "\n";
#endif
}



void
IvGL::print_shader_log(std::ostream& out, const GLuint shader_id)
{
    GLint size = 0;
    glGetShaderiv(shader_id, GL_INFO_LOG_LENGTH, &size);
    if (size > 0) {
        GLchar* log = new GLchar[size];
        glGetShaderInfoLog(shader_id, size, NULL, log);
        out << "compile log:\n" << log << "---\n";
        delete[] log;
    }
}



void
IvGL::check_gl_extensions(void)
{
    m_use_shaders = hasOpenGLFeature(QOpenGLFunctions::Shaders);

    QOpenGLContext* context = QOpenGLContext::currentContext();
    QSurfaceFormat format   = context->format();
    bool isGLES = format.renderableType() == QSurfaceFormat::OpenGLES;

    m_use_srgb = (isGLES && format.majorVersion() >= 3)
                 || (!isGLES && format.version() >= qMakePair(2, 1))
                 || context->hasExtension("GL_EXT_texture_sRGB")
                 || context->hasExtension("GL_EXT_sRGB");

    m_use_halffloat = (!isGLES && format.version() >= qMakePair(3, 0))
                      || context->hasExtension("GL_ARB_half_float_pixel")
                      || context->hasExtension("GL_NV_half_float_pixel")
                      || context->hasExtension("GL_OES_texture_half_float");

    m_use_float = (!isGLES && format.version() >= qMakePair(3, 0))
                  || context->hasExtension("GL_ARB_texture_float")
                  || context->hasExtension("GL_ATI_texture_float")
                  || context->hasExtension("GL_OES_texture_float");

    m_max_texture_size = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &m_max_texture_size);
    // FIXME: Need a smarter way to handle (video) memory.
    // Don't assume that systems capable of using 8k^2 textures have enough
    // resources to use more than one of those at the same time.
    m_max_texture_size = std::min(m_max_texture_size, 4096);

#ifndef NDEBUG
    // Report back...
    std::cerr << "OpenGL Shading Language supported: " << m_use_shaders << "\n";
    std::cerr << "OpenGL sRGB color space textures supported: " << m_use_srgb
              << "\n";
    std::cerr << "OpenGL half-float pixels supported: " << m_use_halffloat
              << "\n";
    std::cerr << "OpenGL float texture storage supported: " << m_use_float
              << "\n";
    std::cerr << "OpenGL max texture dimension: " << m_max_texture_size << "\n";
#endif
}



void
IvGL::typespec_to_opengl(const ImageSpec& spec, int nchannels, GLenum& gltype,
                         GLenum& glformat, GLenum& glinternalformat) const
{
    switch (spec.format.basetype) {
    case TypeDesc::FLOAT: gltype = GL_FLOAT; break;
    case TypeDesc::HALF:
        if (m_use_halffloat) {
            gltype = GL_HALF_FLOAT_ARB;
        } else {
            // If we reach here then something really wrong happened: When
            // half-float is not supported, the image should be loaded as
            // UINT8 (no GLSL support) or FLOAT (GLSL support).
            // See IvImage::loadCurrentImage()
            std::cerr << "Tried to load an unsupported half-float image.\n";
            gltype = GL_INVALID_ENUM;
        }
        break;
    case TypeDesc::INT: gltype = GL_INT; break;
    case TypeDesc::UINT: gltype = GL_UNSIGNED_INT; break;
    case TypeDesc::INT16: gltype = GL_SHORT; break;
    case TypeDesc::UINT16: gltype = GL_UNSIGNED_SHORT; break;
    case TypeDesc::INT8: gltype = GL_BYTE; break;
    case TypeDesc::UINT8: gltype = GL_UNSIGNED_BYTE; break;
    default:
        gltype = GL_UNSIGNED_BYTE;  // punt
        break;
    }

    bool issrgb = Strutil::iequals(spec.get_string_attribute("oiio:ColorSpace"),
                                   "sRGB");

    glinternalformat = nchannels;
    if (nchannels == 1) {
        glformat = GL_LUMINANCE;
        if (m_use_srgb && issrgb) {
            if (spec.format.basetype == TypeDesc::UINT8) {
                glinternalformat = GL_SLUMINANCE8;
            } else {
                glinternalformat = GL_SLUMINANCE;
            }
        } else if (spec.format.basetype == TypeDesc::UINT8) {
            glinternalformat = GL_LUMINANCE8;
        } else if (spec.format.basetype == TypeDesc::UINT16) {
            glinternalformat = GL_LUMINANCE16;
        } else if (m_use_float && spec.format.basetype == TypeDesc::FLOAT) {
            glinternalformat = GL_LUMINANCE32F_ARB;
        } else if (m_use_float && spec.format.basetype == TypeDesc::HALF) {
            glinternalformat = GL_LUMINANCE16F_ARB;
        }
    } else if (nchannels == 2) {
        glformat = GL_LUMINANCE_ALPHA;
        if (m_use_srgb && issrgb) {
            if (spec.format.basetype == TypeDesc::UINT8) {
                glinternalformat = GL_SLUMINANCE8_ALPHA8;
            } else {
                glinternalformat = GL_SLUMINANCE_ALPHA;
            }
        } else if (spec.format.basetype == TypeDesc::UINT8) {
            glinternalformat = GL_LUMINANCE8_ALPHA8;
        } else if (spec.format.basetype == TypeDesc::UINT16) {
            glinternalformat = GL_LUMINANCE16_ALPHA16;
        } else if (m_use_float && spec.format.basetype == TypeDesc::FLOAT) {
            glinternalformat = GL_LUMINANCE_ALPHA32F_ARB;
        } else if (m_use_float && spec.format.basetype == TypeDesc::HALF) {
            glinternalformat = GL_LUMINANCE_ALPHA16F_ARB;
        }
    } else if (nchannels == 3) {
        glformat = GL_RGB;
        if (m_use_srgb && issrgb) {
            if (spec.format.basetype == TypeDesc::UINT8) {
                glinternalformat = GL_SRGB8;
            } else {
                glinternalformat = GL_SRGB;
            }
        } else if (spec.format.basetype == TypeDesc::UINT8) {
            glinternalformat = GL_RGB8;
        } else if (spec.format.basetype == TypeDesc::UINT16) {
            glinternalformat = GL_RGB16;
        } else if (m_use_float && spec.format.basetype == TypeDesc::FLOAT) {
            glinternalformat = GL_RGB32F_ARB;
        } else if (m_use_float && spec.format.basetype == TypeDesc::HALF) {
            glinternalformat = GL_RGB16F_ARB;
        }
    } else if (nchannels == 4) {
        glformat = GL_RGBA;
        if (m_use_srgb && issrgb) {
            if (spec.format.basetype == TypeDesc::UINT8) {
                glinternalformat = GL_SRGB8_ALPHA8;
            } else {
                glinternalformat = GL_SRGB_ALPHA;
            }
        } else if (spec.format.basetype == TypeDesc::UINT8) {
            glinternalformat = GL_RGBA8;
        } else if (spec.format.basetype == TypeDesc::UINT16) {
            glinternalformat = GL_RGBA16;
        } else if (m_use_float && spec.format.basetype == TypeDesc::FLOAT) {
            glinternalformat = GL_RGBA32F_ARB;
        } else if (m_use_float && spec.format.basetype == TypeDesc::HALF) {
            glinternalformat = GL_RGBA16F_ARB;
        }
    } else {
        glformat         = GL_INVALID_ENUM;
        glinternalformat = GL_INVALID_ENUM;
    }
}



void
IvGL::load_texture(int x, int y, int width, int height)
{
    const ImageSpec& spec = m_current_image->spec();
    // Find if this has already been loaded.
    for (auto&& tb : m_texbufs) {
        if (tb.x == x && tb.y == y && tb.width >= width
            && tb.height >= height) {
            glBindTexture(GL_TEXTURE_2D, tb.tex_object);
            return;
        }
    }

    setCursor(Qt::WaitCursor);

    int nchannels = spec.nchannels;
    // For simplicity, we don't support more than 4 channels without shaders
    // (yet).
    if (m_use_shaders) {
        nchannels = num_channels(m_viewer.current_channel(), nchannels,
                                 m_viewer.current_color_mode());
    }
    GLenum gltype, glformat, glinternalformat;
    typespec_to_opengl(spec, nchannels, gltype, glformat, glinternalformat);

    TexBuffer& tb = m_texbufs[m_last_texbuf_used];
    tb.x          = x;
    tb.y          = y;
    tb.width      = width;
    tb.height     = height;
    // Copy the imagebuf pixels we need, that's the only way we can do
    // it safely since ImageBuf has a cache underneath and the whole image
    // may not be resident at once.
    if (!m_use_shaders) {
        m_current_image->get_pixels(ROI(x, x + width, y, y + height),
                                    spec.format,
                                    as_writable_bytes(make_span(m_tex_buffer)));
    } else {
        m_current_image->get_pixels(ROI(x, x + width, y, y + height, 0, 1,
                                        m_viewer.current_channel(),
                                        m_viewer.current_channel() + nchannels),
                                    spec.format,
                                    as_writable_bytes(make_span(m_tex_buffer)));
    }

    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, m_pbo_objects[m_last_pbo_used]);
    glBufferData(GL_PIXEL_UNPACK_BUFFER,
                 GLsizeiptr(uint64_t(width) * uint64_t(height)
                            * uint64_t(nchannels)
                            * uint64_t(spec.format.size())),
                 &m_tex_buffer[0], GL_STREAM_DRAW);
    print_error("After buffer data");
    m_last_pbo_used = (m_last_pbo_used + 1) & 1;

    // When using PBO this is the offset within the buffer.
    void* data = 0;

    glBindTexture(GL_TEXTURE_2D, tb.tex_object);
    print_error("After bind texture");
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, glformat, gltype,
                    data);
    print_error("After loading sub image");
    m_last_texbuf_used = (m_last_texbuf_used + 1) % m_texbufs.size();
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
}



bool
IvGL::is_too_big(float width, float height)
{
    unsigned int tiles = (unsigned int)(ceilf(width / m_max_texture_size)
                                        * ceilf(height / m_max_texture_size));
    return tiles > m_texbufs.size();
}



void
IvGL::update_state(void)
{
    create_shaders();
}



void
IvGL::print_error(const char* msg)
{
    for (GLenum err = glGetError(); err != GL_NO_ERROR; err = glGetError())
        std::cerr << "GL error " << msg << " " << (int)err << " - "
                  << gl_err_to_string(err) << "\n";
}

OIIO_PRAGMA_WARNING_POP
