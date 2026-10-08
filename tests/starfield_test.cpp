#include "shaders.hpp"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

int assertions = 0;

void check(bool condition, std::string_view description) {
    ++assertions;
    if (!condition) throw std::runtime_error(std::string(description));
}

struct Unavailable : std::runtime_error {
    using std::runtime_error::runtime_error;
};

class Context {
  public:
    Context() {
        const auto platformDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
            eglGetProcAddress("eglGetPlatformDisplayEXT"));
        if (platformDisplay)
            display = platformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
        if (display == EGL_NO_DISPLAY) display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        EGLint major = 0, minor = 0;
        if (display == EGL_NO_DISPLAY || !eglInitialize(display, &major, &minor))
            throw Unavailable("No offscreen EGL display is available");
        if (!eglBindAPI(EGL_OPENGL_ES_API)) throw Unavailable("EGL cannot bind OpenGL ES");
        constexpr EGLint configAttributes[] = {
            EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
            EGL_NONE,
        };
        EGLConfig config{};
        EGLint count = 0;
        if (!eglChooseConfig(display, configAttributes, &config, 1, &count) || count != 1)
            throw Unavailable("No OpenGL ES 3 pbuffer configuration is available");
        constexpr EGLint surfaceAttributes[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
        surface = eglCreatePbufferSurface(display, config, surfaceAttributes);
        constexpr EGLint contextAttributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttributes);
        if (surface == EGL_NO_SURFACE || context == EGL_NO_CONTEXT ||
            !eglMakeCurrent(display, surface, surface, context))
            throw Unavailable("An offscreen OpenGL ES 3 context cannot be created");
        std::cout << "GLES renderer: " << glGetString(GL_RENDERER) << '\n';
    }
    ~Context() {
        if (display == EGL_NO_DISPLAY) return;
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
        if (surface != EGL_NO_SURFACE) eglDestroySurface(display, surface);
        eglTerminate(display);
    }
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

  private:
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLSurface surface = EGL_NO_SURFACE;
    EGLContext context = EGL_NO_CONTEXT;
};

GLuint compile(GLenum type, const char* source) {
    const GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint successful = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &successful);
    if (successful != GL_TRUE) {
        std::array<char, 8192> log{};
        glGetShaderInfoLog(shader, static_cast<GLsizei>(log.size()), nullptr, log.data());
        glDeleteShader(shader);
        throw std::runtime_error(std::string("Production shader compilation failed: ") + log.data());
    }
    return shader;
}

using Pixel = std::array<std::uint8_t, 4>;
struct Image {
    int width = 0, height = 0;
    std::vector<Pixel> pixels;
    const Pixel& at(int x, int y) const { return pixels.at(static_cast<std::size_t>(y * width + x)); }
    bool operator==(const Image&) const = default;
};
struct Frame {
    Image normal, mirror;
};
struct Settings {
    int width = 640, height = 360;
    float scale = 1.0F, time = 0.0F, stars = 120.0F, background = 1.0F;
    std::uint32_t seed = 0xC05C1CU;
    std::array<float, 4> sky = {0.0F, 0.0F, 640.0F, 360.0F};
    std::array<float, 4> clip = {0.0F, 0.0F, 640.0F, 360.0F};
    std::array<float, 4> clear = {0.7F, 0.2F, 0.5F, 1.0F};
};

class Renderer {
  public:
    Renderer() {
        const GLuint vertex = compile(GL_VERTEX_SHADER, cosmic::vertexShaderSource);
        const GLuint fragment = compile(GL_FRAGMENT_SHADER, cosmic::fragmentShaderSource);
        program = glCreateProgram();
        glAttachShader(program, vertex);
        glAttachShader(program, fragment);
        glLinkProgram(program);
        glDeleteShader(vertex);
        glDeleteShader(fragment);
        GLint linked = GL_FALSE;
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
        if (linked != GL_TRUE) {
            std::array<char, 8192> log{};
            glGetProgramInfoLog(program, static_cast<GLsizei>(log.size()), nullptr, log.data());
            throw std::runtime_error(std::string("Production shader link failed: ") + log.data());
        }
        constexpr std::array<float, 12> vertices = {0, 0, 0, 1, 1, 0, 1, 0, 0, 1, 1, 1};
        glGenVertexArrays(1, &vao);
        glGenBuffers(1, &vbo);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
        glGenFramebuffers(1, &framebuffer);
        glGenTextures(2, textures.data());
        glUseProgram(program);
        // Projection and quad conventions are the same as the compositor pass.
        constexpr std::array<float, 9> projection = {2, 0, 0, 0, 2, 0, -1, -1, 1};
        glUniformMatrix3fv(uniform("uProjection"), 1, GL_FALSE, projection.data());
        glUniform1f(uniform("uAngle"), 0);
        glUniform1f(uniform("uStretch"), 1);
        glUniform1f(uniform("uTwist"), 0);
        glUniform1f(uniform("uBend"), 0);
        glUniform4f(uniform("uCrop"), 0, 0, 1, 1);
        glUniform1i(uniform("uMode"), 0);
        check(glGetError() == GL_NO_ERROR, "shader and quad initialization has no GL errors");
    }
    ~Renderer() {
        glDeleteTextures(2, textures.data());
        glDeleteFramebuffers(1, &framebuffer);
        glDeleteBuffers(1, &vbo);
        glDeleteVertexArrays(1, &vao);
        glDeleteProgram(program);
    }
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    Frame draw(const Settings& settings) {
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        for (std::size_t index = 0; index < textures.size(); ++index) {
            glBindTexture(GL_TEXTURE_2D, textures[index]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, settings.width, settings.height,
                         0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + index,
                                   GL_TEXTURE_2D, textures[index], 0);
        }
        constexpr std::array<GLenum, 2> attachments = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
        glDrawBuffers(static_cast<GLsizei>(attachments.size()), attachments.data());
        check(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE,
              "two-attachment screencopy framebuffer is complete");
        glViewport(0, 0, settings.width, settings.height);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        for (GLint attachment = 0; attachment < 2; ++attachment)
            glClearBufferfv(GL_COLOR, attachment, settings.clear.data());
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glBlendEquation(GL_FUNC_ADD);
        glUseProgram(program);
        glBindVertexArray(vao);
        glUniform2f(uniform("uResolution"), settings.width, settings.height);
        glUniform2f(uniform("uCenter"), settings.width * 0.5F, settings.height * 0.5F);
        glUniform2f(uniform("uSize"), settings.width, settings.height);
        glUniform1f(uniform("uTime"), settings.time);
        glUniform1f(uniform("uStars"), settings.stars);
        glUniform1f(uniform("uBackground"), settings.background);
        glUniform1f(uniform("uPixelScale"), settings.scale);
        glUniform1ui(uniform("uSeed"), settings.seed);
        glUniform4fv(uniform("uSkyViewport"), 1, settings.sky.data());
        glUniform4fv(uniform("uClip"), 1, settings.clip.data());
        glDrawArrays(GL_TRIANGLES, 0, 6);
        Frame frame;
        frame.normal = read(GL_COLOR_ATTACHMENT0, settings.width, settings.height);
        frame.mirror = read(GL_COLOR_ATTACHMENT1, settings.width, settings.height);
        check(glGetError() == GL_NO_ERROR, "rendering and readback have no GL errors");
        check(frame.normal == frame.mirror, "normal and Hyprland screencopy mirror outputs are identical");
        return frame;
    }

  private:
    GLint uniform(const char* name) const {
        const GLint location = glGetUniformLocation(program, name);
        if (location < 0) throw std::runtime_error(std::string("Missing production uniform: ") + name);
        return location;
    }
    static Image read(GLenum attachment, int width, int height) {
        Image result{width, height, std::vector<Pixel>(static_cast<std::size_t>(width * height))};
        glReadBuffer(attachment);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, result.pixels.data());
        return result;
    }
    GLuint program = 0, vao = 0, vbo = 0, framebuffer = 0;
    std::array<GLuint, 2> textures{};
};

std::vector<bool> starMask(const Image& stars, const Image& base) {
    check(stars.width == base.width && stars.height == base.height, "star masks compare equal-sized canvases");
    std::vector<bool> result(stars.pixels.size());
    for (std::size_t index = 0; index < result.size(); ++index) {
        int difference = 0;
        for (int channel = 0; channel < 3; ++channel)
            difference = std::max(difference, int(stars.pixels[index][channel]) - int(base.pixels[index][channel]));
        result[index] = difference > 12;
    }
    return result;
}

// A CPU oracle deliberately renders the actual glyph bitmap, not a shader-text
// assertion or a few hand-picked screenshots. Pixel coverage includes the
// scale-aware antialiasing used on physical 1x and 2x displays.
std::uint32_t hash(std::uint32_t value) {
    value ^= value >> 16U;
    value *= 0x7feb352dU;
    value ^= value >> 15U;
    value *= 0x846ca68bU;
    return value ^ (value >> 16U);
}
float random(std::uint32_t value) { return float(hash(value) >> 8U) / 16777216.0F; }
float smooth(float low, float high, float value) {
    const float phase = std::clamp((value - low) / (high - low), 0.0F, 1.0F);
    return phase * phase * (3.0F - 2.0F * phase);
}
float glyphCoverage(float x, float y, int glyph, float size, float scale) {
    constexpr std::array<std::array<std::uint32_t, 7>, 4> bitmaps = {{
        {0, 0, 0, 4, 4, 0, 0},
        {0, 4, 4, 31, 4, 4, 0},
        {0, 17, 10, 31, 10, 17, 0},
        {0, 1, 2, 4, 8, 16, 0},
    }};
    const float inkX = x / size + 2.5F, inkY = y / size + 3.5F;
    const int column = static_cast<int>(std::floor(inkX));
    const int row = static_cast<int>(std::floor(inkY));
    if (column < 0 || column >= 5 || row < 0 || row >= 7 ||
        (bitmaps.at(glyph).at(row) & (1U << column)) == 0)
        return 0;
    const float edgeX = std::min(inkX - std::floor(inkX), 1.0F - (inkX - std::floor(inkX)));
    const float edgeY = std::min(inkY - std::floor(inkY), 1.0F - (inkY - std::floor(inkY)));
    const float antialias = std::max(0.65F / size / scale, 0.001F);
    return smooth(0, antialias, edgeX) * smooth(0, antialias, edgeY);
}
using Color = std::array<float, 3>;
Color starColor(float x, float y, const Settings& settings) {
    const auto column = static_cast<std::uint32_t>(std::floor(x / 34.0F));
    const auto row = static_cast<std::uint32_t>(std::floor(y / 30.0F));
    const auto key = hash(column ^ row * 0x9e3779b9U ^ settings.seed);
    const float canvasWidth = settings.sky[2] / settings.scale;
    const float canvasHeight = settings.sky[3] / settings.scale;
    const float slots = std::max(1.0F, std::ceil(canvasWidth / 34.0F - 0.0001F)) *
                        std::max(1.0F, std::ceil(canvasHeight / 30.0F - 0.0001F));
    if (settings.stars <= 0 || random(key) >= std::min(1.0F, settings.stars / slots)) return {};
    const float centerX = (float(column) + 0.5F) * 34.0F + (random(key + 1U) - 0.5F) * 18.0F;
    const float centerY = (float(row) + 0.5F) * 30.0F + (random(key + 2U) - 0.5F) * 12.0F;
    const float tier = random(key + 3U);
    const int glyph = tier < 0.38F ? 0 : (tier < 0.76F ? 1 : 2);
    constexpr std::array<float, 3> sizes = {1.2F, 1.45F, 1.7F};
    const float coverage = glyphCoverage(x - centerX, y - centerY, glyph, sizes[glyph], settings.scale);
    const float brightness = 0.32F + (0.86F - 0.32F) * tier;
    const float twinkle = 0.8F + 0.2F * std::sin(settings.time * (0.45F + random(key + 4U) * 0.65F) +
                                               random(key + 5U) * 6.2831853F);
    Color tint = {0.51F + (0.80F - 0.51F) * tier, 0.67F + (0.89F - 0.67F) * tier,
                  0.90F + (1.0F - 0.90F) * tier};
    if (random(key + 6U) > 0.94F) tint = {0.90F, 0.78F, 0.62F};
    for (auto& channel : tint) channel *= brightness * twinkle * coverage;
    return tint;
}
float meteorOnset(const Settings& settings, std::uint32_t epoch = 0) {
    return float(epoch) * 12.0F + 3.0F + random(hash(epoch ^ settings.seed ^ 0x4d455445U)) * 4.0F;
}
Color meteorColor(float x, float y, const Settings& settings) {
    if (settings.stars <= 0) return {};
    const auto epoch = static_cast<std::uint32_t>(std::floor(settings.time / 12.0F));
    const auto key = hash(epoch ^ settings.seed ^ 0x4d455445U);
    const float age = settings.time - meteorOnset(settings, epoch);
    if (age < 0 || age > 1.8F) return {};
    const float width = settings.sky[2] / settings.scale;
    const float height = settings.sky[3] / settings.scale;
    const float travelX = width * 0.52F, travelY = height * 0.24F;
    const float distance = std::hypot(travelX, travelY);
    const float directionX = travelX / distance, directionY = travelY / distance;
    const float headX = width * (0.12F + random(key + 1U) * 0.20F) + travelX * age / 1.8F;
    const float headY = height * (0.12F + random(key + 2U) * 0.20F) + travelY * age / 1.8F;
    const float segment = std::floor(((headX - x) * directionX + (headY - y) * directionY) / 12.0F + 0.5F);
    if (segment < 0 || segment > 9) return {};
    const int glyph = segment == 0 ? 2 : (segment < 6 ? 3 : 0);
    const float ink = segment == 0 ? 1.8F : 1.35F;
    const float coverage = glyphCoverage(x - (headX - directionX * segment * 12.0F),
                                        y - (headY - directionY * segment * 12.0F), glyph, ink, settings.scale);
    const float envelope = smooth(0, 0.16F, age) * (1.0F - smooth(1.25F, 1.8F, age));
    const float tail = std::pow(1.0F - segment / 10.0F, 1.6F);
    return {0.70F * coverage * envelope * tail, 0.87F * coverage * envelope * tail,
            coverage * envelope * tail};
}
void checkGlyphPixels(const Image& actual, const Image& base, const Settings& settings) {
    for (int y = 0; y < actual.height; ++y)
        for (int x = 0; x < actual.width; ++x) {
            const float logicalX = (float(x) + 0.5F - settings.sky[0]) / settings.scale;
            const float logicalY = (float(y) + 0.5F - settings.sky[1]) / settings.scale;
            const auto star = starColor(logicalX, logicalY, settings);
            const auto meteor = meteorColor(logicalX, logicalY, settings);
            for (int channel = 0; channel < 3; ++channel) {
                const int expected = static_cast<int>(std::lround(std::clamp(
                    float(base.at(x, y)[channel]) + 255.0F * (star[channel] + meteor[channel]), 0.0F, 255.0F)));
                if (std::abs(int(actual.at(x, y)[channel]) - expected) > 2) {
                    std::cerr << "oracle mismatch at " << x << ',' << y << " channel " << channel
                              << " scale " << settings.scale << " time " << settings.time
                              << ": actual=" << int(actual.at(x, y)[channel]) << " expected=" << expected
                              << " base=" << int(base.at(x, y)[channel]) << '\n';
                }
                check(std::abs(int(actual.at(x, y)[channel]) - expected) <= 2,
                      "production pixels match independent 5x7 ASCII bitmap, twinkle and meteor oracle");
            }
        }
}

void backgroundAndDeterminism(Renderer& renderer) {
    Settings settings;
    const auto original = renderer.draw(settings).normal;
    settings.clear = {0.0F, 1.0F, 0.0F, 1.0F};
    const auto changedWallpaper = renderer.draw(settings).normal;
    check(original == changedWallpaper, "opaque Cosmic sky completely hides the existing wallpaper");
    check(std::ranges::all_of(original.pixels, [](const Pixel& pixel) { return pixel[3] == 255; }),
          "default sky is opaque across the entire canvas");
    std::uint64_t totalBrightness = 0;
    for (const auto& pixel : original.pixels)
        totalBrightness += pixel[0] + pixel[1] + pixel[2];
    check(totalBrightness < original.pixels.size() * 75U, "space sky remains dark instead of washing out windows");
    check(original == renderer.draw(settings).normal, "same seed and time produce byte-identical rendered sky");
    ++settings.seed;
    check(original != renderer.draw(settings).normal, "different seeds produce distinct rendered starfields");
    settings.seed = 0xC05C1CU;
    settings.time = 1.0F;
    check(original != renderer.draw(settings).normal, "stars actually twinkle over time");
    settings.stars = 0;
    settings.time = 0;
    const auto noStars = renderer.draw(settings).normal;
    for (const float time : {4.0F, 6.0F, 9.0F, 16.0F, 29.0F, 53.0F}) {
        settings.time = time;
        check(noStars == renderer.draw(settings).normal, "zero stars disables both twinkle and shooting stars");
    }
    settings.background = 0.5F;
    settings.clear = {0, 0, 0, 0};
    const auto translucent = renderer.draw(settings).normal;
    for (std::size_t index = 0; index < translucent.pixels.size(); ++index) {
        const auto& pixel = translucent.pixels[index];
        check(pixel[3] == 128, "explicit half-opacity sky preserves alpha");
        for (int channel = 0; channel < 3; ++channel)
            check(std::abs(int(pixel[channel]) * 2 - int(noStars.pixels[index][channel])) <= 2,
                  "translucent background uses premultiplied RGB");
    }
}

void logicalDpiAndInset(Renderer& renderer) {
    Settings settings;
    settings.time = 1.0F;
    const auto logical = renderer.draw(settings).normal;
    settings.stars = 0;
    const auto logicalBase = renderer.draw(settings).normal;
    const auto logicalMask = starMask(logical, logicalBase);
    check(std::ranges::count(logicalMask, true) > 50, "ASCII star pixels are visibly rendered");
    settings.stars = 120;
    checkGlyphPixels(logical, logicalBase, settings);
    settings.width *= 2;
    settings.height *= 2;
    settings.scale = 2;
    settings.sky = {0, 0, float(settings.width), float(settings.height)};
    settings.clip = settings.sky;
    settings.stars = 120;
    const auto hidpi = renderer.draw(settings).normal;
    settings.stars = 0;
    const auto hidpiBase = renderer.draw(settings).normal;
    const auto hidpiMask = starMask(hidpi, hidpiBase);
    settings.stars = 120;
    checkGlyphPixels(hidpi, hidpiBase, settings);
    check(std::ranges::count(hidpiMask, true) > std::ranges::count(logicalMask, true) * 3,
          "2x keeps readable logical-size glyphs instead of shrinking them to physical-size stars");

    settings = Settings{};
    settings.time = 1;
    const auto local = renderer.draw(settings).normal;
    settings.width = 800;
    settings.height = 480;
    constexpr int left = 80, bottom = 60;
    settings.sky = {left, bottom, 640, 360};
    settings.clip = {left, bottom, left + 640, bottom + 360};
    settings.clear = {0, 1, 0, 1};
    const auto inset = renderer.draw(settings).normal;
    for (int y = 0; y < inset.height; ++y)
        for (int x = 0; x < inset.width; ++x) {
            if (x >= left && x < left + local.width && y >= bottom && y < bottom + local.height)
                for (int channel = 0; channel < 4; ++channel)
                    check(std::abs(int(inset.at(x, y)[channel]) - int(local.at(x - left, y - bottom)[channel])) <= 1,
                          "peer-canvas origin changes do not re-seed or distort the local sky");
            else
                check(inset.at(x, y) == Pixel{0, 255, 0, 255}, "sky is clipped to the peer content canvas");
        }
}

void meteorLifecycle(Renderer& renderer) {
    Settings settings;
    settings.stars = 0;
    const auto base = renderer.draw(settings).normal;
    settings.stars = 120;
    const float onset = meteorOnset(settings);
    check(onset >= 3 && onset < 7, "shooting star waits several seconds after entering Cosmic mode");
    for (const float age : {-0.1F, 0.6F, 1.0F, 1.9F}) {
        settings.time = onset + age;
        const auto actual = renderer.draw(settings).normal;
        checkGlyphPixels(actual, base, settings);
        int litMeteorPixels = 0;
        for (int y = 0; y < actual.height; ++y)
            for (int x = 0; x < actual.width; ++x)
                if (meteorColor(float(x) + 0.5F, float(y) + 0.5F, settings)[2] > 0.03F)
                    ++litMeteorPixels;
        check(age > 0 && age < 1.8F ? litMeteorPixels > 20 : litMeteorPixels == 0,
              "meteor has an actual ASCII head and tail only during its bounded lifetime");
    }
    settings.time = onset + 0.6F;
    const auto firstPosition = renderer.draw(settings).normal;
    const Settings firstSettings = settings;
    settings.time = onset + 1.0F;
    const auto nextPosition = renderer.draw(settings).normal;
    const auto meteorCenter = [&](const Image& image, const Settings& frame) {
        double weight = 0, weightedX = 0, weightedY = 0;
        for (int y = 0; y < image.height; ++y)
            for (int x = 0; x < image.width; ++x) {
                const auto star = starColor(float(x) + 0.5F, float(y) + 0.5F, frame);
                const double residual = int(image.at(x, y)[2]) - int(base.at(x, y)[2]) - star[2] * 255.0;
                if (residual <= 8) continue;
                weight += residual;
                weightedX += residual * x;
                weightedY += residual * y;
            }
        check(weight > 100, "rendered meteor remains visible after removing static stars and their twinkle");
        return std::array<double, 2>{weightedX / weight, weightedY / weight};
    };
    const auto firstCenter = meteorCenter(firstPosition, firstSettings);
    const auto nextCenter = meteorCenter(nextPosition, settings);
    check(nextCenter[0] - firstCenter[0] > 60 && nextCenter[1] - firstCenter[1] > 10,
          "actual meteor pixels move diagonally independently of ordinary star twinkle");
}

} // namespace

int main() {
    try {
        Context context;
        Renderer renderer;
        backgroundAndDeterminism(renderer);
        logicalDpiAndInset(renderer);
        meteorLifecycle(renderer);
        std::cout << "starfield: " << assertions << " rendered-pixel checks passed\n";
        return EXIT_SUCCESS;
    } catch (const Unavailable& error) {
        std::cout << "SKIP: " << error.what() << '\n';
        return 77;
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
