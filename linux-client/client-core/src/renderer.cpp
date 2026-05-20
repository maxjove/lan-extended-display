#include "led/client/renderer.h"

#include "led/common/logger.h"

#include <algorithm>
#include <cstddef>
#include <cstring>

#if defined(LED_HAS_X11)
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#if defined(LED_HAS_GLX)
#ifndef GL_GLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES
#endif
#include <GL/gl.h>
#include <GL/glx.h>
#endif
#if defined(LED_HAS_XSHM)
#include <X11/extensions/XShm.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#endif
#if defined(Status)
#undef Status
#endif
#endif

namespace led::client {

#if defined(LED_HAS_X11) && defined(LED_HAS_XSHM)
namespace {

void destroySharedImage(Display* display, void*& imageStorage, void*& infoStorage, std::uint64_t& bytes, bool& attached) {
    auto* image = static_cast<XImage*>(imageStorage);
    auto* info = static_cast<XShmSegmentInfo*>(infoStorage);
    if (display != nullptr && info != nullptr && attached) {
        XShmDetach(display, info);
        XSync(display, False);
    }
    if (info != nullptr && info->shmaddr != nullptr && info->shmaddr != reinterpret_cast<char*>(-1)) {
        shmdt(info->shmaddr);
    }
    if (image != nullptr) {
        image->data = nullptr;
        XDestroyImage(image);
    }
    delete info;
    imageStorage = nullptr;
    infoStorage = nullptr;
    bytes = 0;
    attached = false;
}

bool ensureSharedImage(
    Display* display,
    int screen,
    std::uint32_t width,
    std::uint32_t height,
    void*& imageStorage,
    void*& infoStorage,
    std::uint64_t& bytes,
    bool& attached) {
    auto* existing = static_cast<XImage*>(imageStorage);
    if (existing != nullptr &&
        static_cast<std::uint32_t>(existing->width) == width &&
        static_cast<std::uint32_t>(existing->height) == height) {
        return true;
    }

    destroySharedImage(display, imageStorage, infoStorage, bytes, attached);

    auto* info = new XShmSegmentInfo{};
    auto* image = XShmCreateImage(
        display,
        DefaultVisual(display, screen),
        static_cast<unsigned int>(DefaultDepth(display, screen)),
        ZPixmap,
        nullptr,
        info,
        width,
        height);
    if (image == nullptr) {
        delete info;
        return false;
    }

    const auto imageBytes = static_cast<std::uint64_t>(image->bytes_per_line) * static_cast<std::uint64_t>(image->height);
    info->shmid = shmget(IPC_PRIVATE, static_cast<std::size_t>(imageBytes), IPC_CREAT | 0600);
    if (info->shmid < 0) {
        image->data = nullptr;
        XDestroyImage(image);
        delete info;
        return false;
    }

    info->shmaddr = static_cast<char*>(shmat(info->shmid, nullptr, 0));
    if (info->shmaddr == reinterpret_cast<char*>(-1)) {
        shmctl(info->shmid, IPC_RMID, nullptr);
        image->data = nullptr;
        XDestroyImage(image);
        delete info;
        return false;
    }

    image->data = info->shmaddr;
    info->readOnly = False;
    if (!XShmAttach(display, info)) {
        shmdt(info->shmaddr);
        shmctl(info->shmid, IPC_RMID, nullptr);
        image->data = nullptr;
        XDestroyImage(image);
        delete info;
        return false;
    }
    shmctl(info->shmid, IPC_RMID, nullptr);
    XSync(display, False);

    imageStorage = image;
    infoStorage = info;
    bytes = imageBytes;
    attached = true;
    return true;
}

}  // namespace
#endif

#if defined(LED_HAS_X11) && defined(LED_HAS_GLX)
namespace {

XVisualInfo* chooseGlVisual(Display* display, int screen) {
    int attributes[] = {
        GLX_RGBA,
        GLX_DOUBLEBUFFER,
        GLX_RED_SIZE,
        8,
        GLX_GREEN_SIZE,
        8,
        GLX_BLUE_SIZE,
        8,
        None,
    };
    return glXChooseVisual(display, screen, attributes);
}

void setupGlState(std::uint32_t width, std::uint32_t height) {
    glViewport(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0.0, 1.0, 1.0, 0.0, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glClearColor(0.0F, 0.0F, 0.0F, 1.0F);
}

void drawLocalCursor(double normalizedX, double normalizedY, std::uint32_t width, std::uint32_t height) {
    const auto x = static_cast<GLfloat>(std::clamp(normalizedX, 0.0, 1.0));
    const auto y = static_cast<GLfloat>(std::clamp(normalizedY, 0.0, 1.0));
    const auto w = static_cast<GLfloat>(18.0 / static_cast<double>(std::max<std::uint32_t>(1, width)));
    const auto h = static_cast<GLfloat>(28.0 / static_cast<double>(std::max<std::uint32_t>(1, height)));
    const auto stem = static_cast<GLfloat>(7.0 / static_cast<double>(std::max<std::uint32_t>(1, width)));

    glUseProgram(0);
    glDisable(GL_TEXTURE_2D);
    glLineWidth(2.0F);
    glColor3f(0.0F, 0.0F, 0.0F);
    glBegin(GL_LINE_LOOP);
    glVertex2f(x, y);
    glVertex2f(x, y + h);
    glVertex2f(x + stem, y + h * 0.72F);
    glVertex2f(x + w, y + h * 0.72F);
    glEnd();

    glColor3f(1.0F, 1.0F, 1.0F);
    glBegin(GL_TRIANGLES);
    glVertex2f(x, y);
    glVertex2f(x, y + h);
    glVertex2f(x + w, y + h * 0.72F);
    glEnd();
    glColor3f(1.0F, 1.0F, 1.0F);
}

void drawFullFrameQuad() {
    glBegin(GL_QUADS);
    glTexCoord2f(0.0F, 0.0F);
    glVertex2f(0.0F, 0.0F);
    glTexCoord2f(1.0F, 0.0F);
    glVertex2f(1.0F, 0.0F);
    glTexCoord2f(1.0F, 1.0F);
    glVertex2f(1.0F, 1.0F);
    glTexCoord2f(0.0F, 1.0F);
    glVertex2f(0.0F, 1.0F);
    glEnd();
}

bool redrawGlBgrxScene(
    Display* display,
    Window window,
    void* contextStorage,
    unsigned int texture,
    std::uint32_t width,
    std::uint32_t height,
    bool drawCursor,
    double cursorX,
    double cursorY) {
    auto context = static_cast<GLXContext>(contextStorage);
    if (context == nullptr || texture == 0 || !glXMakeCurrent(display, window, context)) {
        return false;
    }
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glEnable(GL_TEXTURE_2D);
    drawFullFrameQuad();
    glDisable(GL_TEXTURE_2D);
    if (drawCursor) {
        drawLocalCursor(cursorX, cursorY, width, height);
    }
    glXSwapBuffers(display, window);
    return glGetError() == GL_NO_ERROR;
}

bool renderGlFrame(
    Display* display,
    Window window,
    void* contextStorage,
    unsigned int& texture,
    std::uint32_t& textureWidth,
    std::uint32_t& textureHeight,
    const std::uint8_t* data,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t stride,
    bool drawCursor,
    double cursorX,
    double cursorY) {
    auto context = static_cast<GLXContext>(contextStorage);
    if (context == nullptr || !glXMakeCurrent(display, window, context)) {
        return false;
    }

    if (texture == 0) {
        GLuint newTexture = 0;
        glGenTextures(1, &newTexture);
        texture = newTexture;
    }

    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<GLint>(stride / 4));

    if (textureWidth != width || textureHeight != height) {
        glTexImage2D(
            GL_TEXTURE_2D,
            0,
            GL_RGBA,
            static_cast<GLsizei>(width),
            static_cast<GLsizei>(height),
            0,
            GL_BGRA,
            GL_UNSIGNED_BYTE,
            nullptr);
        textureWidth = width;
        textureHeight = height;
    }
    glTexSubImage2D(
        GL_TEXTURE_2D,
        0,
        0,
        0,
        static_cast<GLsizei>(width),
        static_cast<GLsizei>(height),
        GL_BGRA,
        GL_UNSIGNED_BYTE,
        data);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

    return redrawGlBgrxScene(display, window, contextStorage, texture, width, height, drawCursor, cursorX, cursorY);
}

GLuint compileShader(GLenum type, const char* source) {
    const GLuint shader = glCreateShader(type);
    if (shader == 0) {
        return 0;
    }
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled != GL_TRUE) {
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

bool ensureYuvProgram(unsigned int& programStorage) {
    if (programStorage != 0) {
        return true;
    }

    constexpr const char* vertexSource =
        "varying vec2 vTex;\n"
        "void main() {\n"
        "  gl_Position = ftransform();\n"
        "  vTex = gl_MultiTexCoord0.st;\n"
        "}\n";
    constexpr const char* fragmentSource =
        "uniform sampler2D texY;\n"
        "uniform sampler2D texU;\n"
        "uniform sampler2D texV;\n"
        "varying vec2 vTex;\n"
        "void main() {\n"
        "  float y = texture2D(texY, vTex).r;\n"
        "  float u = texture2D(texU, vTex).r - 0.5;\n"
        "  float v = texture2D(texV, vTex).r - 0.5;\n"
        "  vec3 rgb;\n"
        "  rgb.r = y + 1.402 * v;\n"
        "  rgb.g = y - 0.344136 * u - 0.714136 * v;\n"
        "  rgb.b = y + 1.772 * u;\n"
        "  gl_FragColor = vec4(rgb, 1.0);\n"
        "}\n";

    const GLuint vertex = compileShader(GL_VERTEX_SHADER, vertexSource);
    const GLuint fragment = compileShader(GL_FRAGMENT_SHADER, fragmentSource);
    if (vertex == 0 || fragment == 0) {
        if (vertex != 0) {
            glDeleteShader(vertex);
        }
        if (fragment != 0) {
            glDeleteShader(fragment);
        }
        return false;
    }

    const GLuint program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);

    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        glDeleteProgram(program);
        return false;
    }

    glUseProgram(program);
    glUniform1i(glGetUniformLocation(program, "texY"), 0);
    glUniform1i(glGetUniformLocation(program, "texU"), 1);
    glUniform1i(glGetUniformLocation(program, "texV"), 2);
    glUseProgram(0);
    programStorage = program;
    return true;
}

void uploadLumaTexture(
    GLenum unit,
    unsigned int& textureStorage,
    std::uint32_t width,
    std::uint32_t height,
    const std::uint8_t* data,
    bool allocate) {
    if (textureStorage == 0) {
        GLuint texture = 0;
        glGenTextures(1, &texture);
        textureStorage = texture;
    }
    glActiveTexture(unit);
    glBindTexture(GL_TEXTURE_2D, textureStorage);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    if (allocate) {
        glTexImage2D(
            GL_TEXTURE_2D,
            0,
            GL_LUMINANCE,
            static_cast<GLsizei>(width),
            static_cast<GLsizei>(height),
            0,
            GL_LUMINANCE,
            GL_UNSIGNED_BYTE,
            data);
        return;
    }
    glTexSubImage2D(
        GL_TEXTURE_2D,
        0,
        0,
        0,
        static_cast<GLsizei>(width),
        static_cast<GLsizei>(height),
        GL_LUMINANCE,
        GL_UNSIGNED_BYTE,
        data);
}

bool renderGlI420Frame(
    Display* display,
    Window window,
    void* contextStorage,
    unsigned int& yTexture,
    unsigned int& uTexture,
    unsigned int& vTexture,
    unsigned int& program,
    std::uint32_t& textureWidth,
    std::uint32_t& textureHeight,
    const std::uint8_t* data,
    std::uint64_t bytes,
    std::uint32_t width,
    std::uint32_t height,
    bool drawCursor,
    double cursorX,
    double cursorY) {
    if ((width % 2) != 0 || (height % 2) != 0) {
        return false;
    }
    const auto yBytes = static_cast<std::uint64_t>(width) * height;
    const auto uvWidth = width / 2;
    const auto uvHeight = height / 2;
    const auto uvBytes = static_cast<std::uint64_t>(uvWidth) * uvHeight;
    if (bytes < yBytes + uvBytes * 2) {
        return false;
    }

    auto context = static_cast<GLXContext>(contextStorage);
    if (context == nullptr || !glXMakeCurrent(display, window, context)) {
        return false;
    }
    if (!ensureYuvProgram(program)) {
        return false;
    }

    const auto* yPlane = data;
    const auto* uPlane = yPlane + yBytes;
    const auto* vPlane = uPlane + uvBytes;
    const bool allocate = textureWidth != width || textureHeight != height;
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    uploadLumaTexture(GL_TEXTURE0, yTexture, width, height, yPlane, allocate);
    uploadLumaTexture(GL_TEXTURE1, uTexture, uvWidth, uvHeight, uPlane, allocate);
    uploadLumaTexture(GL_TEXTURE2, vTexture, uvWidth, uvHeight, vPlane, allocate);
    textureWidth = width;
    textureHeight = height;
    glActiveTexture(GL_TEXTURE0);

    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(program);
    glEnable(GL_TEXTURE_2D);
    glBegin(GL_QUADS);
    glTexCoord2f(0.0F, 0.0F);
    glVertex2f(0.0F, 0.0F);
    glTexCoord2f(1.0F, 0.0F);
    glVertex2f(1.0F, 0.0F);
    glTexCoord2f(1.0F, 1.0F);
    glVertex2f(1.0F, 1.0F);
    glTexCoord2f(0.0F, 1.0F);
    glVertex2f(0.0F, 1.0F);
    glEnd();
    glDisable(GL_TEXTURE_2D);
    glUseProgram(0);
    if (drawCursor) {
        drawLocalCursor(cursorX, cursorY, width, height);
    }
    glXSwapBuffers(display, window);
    return glGetError() == GL_NO_ERROR;
}

bool redrawGlI420Scene(
    Display* display,
    Window window,
    void* contextStorage,
    unsigned int yTexture,
    unsigned int uTexture,
    unsigned int vTexture,
    unsigned int program,
    std::uint32_t width,
    std::uint32_t height,
    bool drawCursor,
    double cursorX,
    double cursorY) {
    auto context = static_cast<GLXContext>(contextStorage);
    if (context == nullptr || yTexture == 0 || uTexture == 0 || vTexture == 0 || program == 0 ||
        !glXMakeCurrent(display, window, context)) {
        return false;
    }

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, yTexture);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, uTexture);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, vTexture);
    glActiveTexture(GL_TEXTURE0);

    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(program);
    glEnable(GL_TEXTURE_2D);
    drawFullFrameQuad();
    glDisable(GL_TEXTURE_2D);
    glUseProgram(0);
    if (drawCursor) {
        drawLocalCursor(cursorX, cursorY, width, height);
    }
    glXSwapBuffers(display, window);
    return glGetError() == GL_NO_ERROR;
}

}  // namespace
#endif

led::Status Renderer::openFullscreen(const protocol::Resolution& resolution) {
    std::lock_guard<std::mutex> lock(mutex_);
    resolution_ = resolution;
    fullscreen_ = true;
#if defined(LED_HAS_X11)
    if (xDisplay_ == nullptr) {
        XInitThreads();
        auto* display = XOpenDisplay(nullptr);
        if (display == nullptr) {
            return Status::unavailable("failed to open X11 display for renderer");
        }

        xDisplay_ = display;
        xScreen_ = DefaultScreen(display);
#if defined(LED_HAS_XSHM)
        xShmAvailable_ = XShmQueryExtension(display) == True;
#endif
        windowWidth_ = std::max<std::uint32_t>(1, resolution.width);
        windowHeight_ = std::max<std::uint32_t>(1, resolution.height);
        const auto root = RootWindow(display, xScreen_);

#if defined(LED_HAS_GLX)
        XVisualInfo* glVisual = chooseGlVisual(display, xScreen_);
        if (glVisual != nullptr) {
            auto glContext = glXCreateContext(display, glVisual, nullptr, True);
            if (glContext != nullptr) {
                XSetWindowAttributes glAttributes{};
                glAttributes.colormap = XCreateColormap(display, root, glVisual->visual, AllocNone);
                glAttributes.background_pixel = 0;
                glAttributes.border_pixel = 0;
                glAttributes.override_redirect = True;
                xWindow_ = XCreateWindow(
                    display,
                    root,
                    0,
                    0,
                    windowWidth_,
                    windowHeight_,
                    0,
                    glVisual->depth,
                    InputOutput,
                    glVisual->visual,
                    CWColormap | CWBackPixel | CWBorderPixel | CWOverrideRedirect,
                    &glAttributes);
                glContext_ = glContext;
                glAvailable_ = true;
            }
            XFree(glVisual);
        }
#endif

        if (xWindow_ == 0) {
        XSetWindowAttributes attributes{};
        attributes.background_pixel = BlackPixel(display, xScreen_);
        attributes.border_pixel = BlackPixel(display, xScreen_);
        attributes.override_redirect = True;
        xWindow_ = XCreateWindow(
            display,
            root,
            0,
            0,
            windowWidth_,
            windowHeight_,
            0,
            CopyFromParent,
            InputOutput,
            CopyFromParent,
            CWBackPixel | CWBorderPixel | CWOverrideRedirect,
            &attributes);
        }
        XStoreName(display, xWindow_, "LAN Extended Display");
        XMapRaised(display, xWindow_);
        XRaiseWindow(display, xWindow_);
        xGc_ = XCreateGC(display, xWindow_, 0, nullptr);
#if defined(LED_HAS_GLX)
        if (glAvailable_) {
            glXMakeCurrent(display, xWindow_, static_cast<GLXContext>(glContext_));
            setupGlState(windowWidth_, windowHeight_);
        }
#endif
        XFlush(display);
    }
#endif
    logInfo("client renderer opened X11 window");
    return Status::ok();
}

led::Status Renderer::close() {
    std::lock_guard<std::mutex> lock(mutex_);
#if defined(LED_HAS_X11)
    if (xDisplay_ != nullptr) {
        auto* display = static_cast<Display*>(xDisplay_);
#if defined(LED_HAS_GLX)
        if (glContext_ != nullptr) {
            glXMakeCurrent(display, xWindow_, static_cast<GLXContext>(glContext_));
            if (glYuvProgram_ != 0) {
                GLuint program = glYuvProgram_;
                glDeleteProgram(program);
                glYuvProgram_ = 0;
            }
            GLuint yuvTextures[] = {glYTexture_, glUTexture_, glVTexture_};
            glDeleteTextures(3, yuvTextures);
            glYTexture_ = 0;
            glUTexture_ = 0;
            glVTexture_ = 0;
            if (glTexture_ != 0) {
                GLuint texture = glTexture_;
                glDeleteTextures(1, &texture);
                glTexture_ = 0;
            }
            glXMakeCurrent(display, None, nullptr);
            glXDestroyContext(display, static_cast<GLXContext>(glContext_));
            glContext_ = nullptr;
            glAvailable_ = false;
        }
#endif
#if defined(LED_HAS_XSHM)
        destroySharedImage(display, xShmImage_, xShmInfo_, xShmBytes_, xShmAttached_);
#endif
        if (xGc_ != nullptr) {
            XFreeGC(display, static_cast<GC>(xGc_));
            xGc_ = nullptr;
        }
        if (xWindow_ != 0) {
            XDestroyWindow(display, xWindow_);
            xWindow_ = 0;
        }
        XCloseDisplay(display);
        xDisplay_ = nullptr;
    }
#endif
    fullscreen_ = false;
    logInfo("client renderer closed");
    return Status::ok();
}

led::Status Renderer::updateLocalCursor(double normalizedX, double normalizedY) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!fullscreen_) {
        return Status::invalidState("cannot update cursor before renderer is fullscreen");
    }

    localCursorX_ = std::clamp(normalizedX, 0.0, 1.0);
    localCursorY_ = std::clamp(normalizedY, 0.0, 1.0);
    localCursorVisible_ = true;
    return Status::ok();
}

led::Status Renderer::submitRawFrame(const RawFrameInfo& frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.rawFrames;
    stats_.bytes += frame.bytes;
    stats_.lastFrame = frame;
    return Status::ok();
}

led::Status Renderer::submitRawFrameData(const RawFrameInfo& frame, const std::uint8_t* data) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.rawFrames;
    stats_.bytes += frame.bytes;
    stats_.lastFrame = frame;

#if defined(LED_HAS_X11)
    if (!fullscreen_ || xDisplay_ == nullptr || xWindow_ == 0 || xGc_ == nullptr || data == nullptr) {
        return Status::ok();
    }
    if (frame.width == 0 || frame.height == 0) {
        return Status::ok();
    }

    auto* display = static_cast<Display*>(xDisplay_);
    const auto width = std::min<std::uint32_t>(frame.width, windowWidth_);
    const auto height = std::min<std::uint32_t>(frame.height, windowHeight_);
    const auto stride = frame.stride != 0 ? frame.stride : frame.width * 4;

#if defined(LED_HAS_GLX)
    if (glAvailable_ &&
        frame.format == "I420" &&
        renderGlI420Frame(
            display,
            xWindow_,
            glContext_,
            glYTexture_,
            glUTexture_,
            glVTexture_,
            glYuvProgram_,
            glYuvWidth_,
            glYuvHeight_,
            data,
            frame.bytes,
            width,
            height,
            localCursorVisible_,
            localCursorX_,
            localCursorY_)) {
        return Status::ok();
    }
    if (glAvailable_ &&
        frame.format == "BGRx" &&
        renderGlFrame(
            display,
            xWindow_,
            glContext_,
            glTexture_,
            glTextureWidth_,
            glTextureHeight_,
            data,
            width,
            height,
            stride,
            localCursorVisible_,
            localCursorX_,
            localCursorY_)) {
        return Status::ok();
    }
#endif

    if (frame.format != "BGRx") {
        return Status::ok();
    }

#if defined(LED_HAS_XSHM)
    if (xShmAvailable_ &&
        ensureSharedImage(display, xScreen_, width, height, xShmImage_, xShmInfo_, xShmBytes_, xShmAttached_)) {
        auto* image = static_cast<XImage*>(xShmImage_);
        const auto* src = data;
        auto* dst = reinterpret_cast<std::uint8_t*>(image->data);
        const auto copyBytes = std::min<std::uint32_t>(stride, static_cast<std::uint32_t>(image->bytes_per_line));
        for (std::uint32_t row = 0; row < height; ++row) {
            std::memcpy(dst + row * image->bytes_per_line, src + row * stride, copyBytes);
        }
        XShmPutImage(display, xWindow_, static_cast<GC>(xGc_), image, 0, 0, 0, 0, width, height, False);
        XFlush(display);
        return Status::ok();
    }
#endif

    auto* image = XCreateImage(
        display,
        DefaultVisual(display, xScreen_),
        static_cast<unsigned int>(DefaultDepth(display, xScreen_)),
        ZPixmap,
        0,
        const_cast<char*>(reinterpret_cast<const char*>(data)),
        width,
        height,
        32,
        static_cast<int>(stride));
    if (image == nullptr) {
        return Status::unavailable("failed to create X11 image");
    }

    XPutImage(display, xWindow_, static_cast<GC>(xGc_), image, 0, 0, 0, 0, width, height);
    XFlush(display);
    image->data = nullptr;
    XDestroyImage(image);
#else
    (void)data;
#endif
    return Status::ok();
}

RendererStats Renderer::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

}  // namespace led::client
