#include "renderer.hpp"
#include "shaders.hpp"

#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <GLES3/gl3.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace cosmic {
namespace {

// Our shader does not use Hyprland's state caches. Restore everything touched so
// subsequent popup, layer, cursor and color-management passes see their state.
class GLState {
  public:
    GLState() {
        glGetIntegerv(GL_CURRENT_PROGRAM, &program);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &buffer);
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpackAlignment);
        glGetIntegerv(GL_UNPACK_ROW_LENGTH, &unpackRowLength);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);
        glActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
        glGetIntegerv(GL_SCISSOR_BOX, scissor.data());
        glGetIntegerv(GL_BLEND_SRC_RGB, &srcRGB);
        glGetIntegerv(GL_BLEND_DST_RGB, &dstRGB);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &srcAlpha);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &dstAlpha);
        glGetIntegerv(GL_BLEND_EQUATION_RGB, &equationRGB);
        glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &equationAlpha);
        blend = glIsEnabled(GL_BLEND);
        scissorEnabled = glIsEnabled(GL_SCISSOR_TEST);
        cull = glIsEnabled(GL_CULL_FACE);
        depth = glIsEnabled(GL_DEPTH_TEST);
    }
    ~GLState() {
        glUseProgram(program);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, buffer);
        glPixelStorei(GL_UNPACK_ALIGNMENT, unpackAlignment);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, unpackRowLength);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture);
        glActiveTexture(activeTexture);
        glScissor(scissor[0], scissor[1], scissor[2], scissor[3]);
        glBlendFuncSeparate(srcRGB, dstRGB, srcAlpha, dstAlpha);
        glBlendEquationSeparate(equationRGB, equationAlpha);
        restore(GL_BLEND, blend);
        restore(GL_SCISSOR_TEST, scissorEnabled);
        restore(GL_CULL_FACE, cull);
        restore(GL_DEPTH_TEST, depth);
    }
  private:
    static void restore(GLenum capability, GLboolean enabled) {
        if (enabled) glEnable(capability); else glDisable(capability);
    }
    GLint program = 0, vao = 0, buffer = 0, activeTexture = GL_TEXTURE0, texture = 0;
    GLint unpackAlignment = 4, unpackRowLength = 0;
    GLint srcRGB = GL_ONE, dstRGB = GL_ZERO, srcAlpha = GL_ONE, dstAlpha = GL_ZERO;
    GLint equationRGB = GL_FUNC_ADD, equationAlpha = GL_FUNC_ADD;
    std::array<GLint, 4> scissor{};
    GLboolean blend = false, scissorEnabled = false, cull = false, depth = false;
};

GLuint compileShader(GLenum type, const char* source) {
    const GLuint shader = glCreateShader(type);
    if (!shader) throw std::runtime_error("Could not create Cosmic GL shader");
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        std::array<char, 2048> log{};
        glGetShaderInfoLog(shader, log.size(), nullptr, log.data());
        glDeleteShader(shader);
        throw std::runtime_error(std::string("Cosmic shader compilation failed: ") + log.data());
    }
    return shader;
}

std::array<float, 3> hue(double value) {
    return {static_cast<float>(0.52 + 0.38 * std::sin(value * 6.2831853)),
            static_cast<float>(0.52 + 0.38 * std::sin(value * 6.2831853 + 2.1)),
            static_cast<float>(0.70 + 0.28 * std::sin(value * 6.2831853 + 4.2))};
}

class CosmicPass final : public IPassElement {
  public:
    CosmicPass(CosmicRenderer& renderer, PHLMONITOR monitor, const Universe& universe,
               const std::vector<Snapshot>& snapshots, bool alternate)
        : m_renderer(renderer), m_monitor(monitor), m_universe(universe),
          m_snapshots(snapshots), m_alternate(alternate) {}
    std::vector<UP<IPassElement>> draw() override {
        if (const auto monitor = m_monitor.lock())
            m_renderer.executeDraw(monitor, m_universe, m_snapshots, m_alternate);
        return {};
    }
    bool needsLiveBlur() override { return false; }
    bool needsPrecomputeBlur() override { return false; }
    const char* passName() override { return "CosmicUniverse"; }
    ePassElementType type() override { return EK_CUSTOM; }
    bool disableSimplification() override { return true; }
    bool undiscardable() override { return true; }
    std::optional<CBox> boundingBox() override {
        if (const auto monitor = m_monitor.lock()) return CBox{{0, 0}, monitor->m_size};
        return std::nullopt;
    }
  private:
    CosmicRenderer& m_renderer;
    PHLMONITORREF m_monitor;
    const Universe& m_universe;
    std::vector<Snapshot> m_snapshots;
    bool m_alternate;
};

} // namespace

bool PeerViewLayout::contains(Vec2 point) const {
    return point.x >= origin.x && point.y >= origin.y &&
           point.x <= origin.x + size.x && point.y <= origin.y + size.y;
}
bool PeerViewLayout::containsContent(Vec2 point) const {
    return point.x >= contentOrigin.x && point.y >= contentOrigin.y &&
           point.x <= contentOrigin.x + contentSize.x && point.y <= contentOrigin.y + contentSize.y;
}
Vec2 PeerViewLayout::toRegionScreen(Vec2 point) const {
    return {region.x + (point.x - contentOrigin.x) / scale,
            region.y + (point.y - contentOrigin.y) / scale};
}
Vec2 PeerViewLayout::fromRegionScreen(Vec2 point) const {
    return contentOrigin + (point - Vec2{region.x, region.y}) * scale;
}
std::optional<PeerViewLayout> peerViewLayout(const Universe& universe, int mainRegion) {
    if (!universe.config().wormholes) return std::nullopt;
    const int peerId = mainRegion >= 1000000 ? mainRegion - 1000000 : mainRegion + 1000000;
    const auto main = std::ranges::find_if(universe.regions(), [=](const Region& r) { return r.id == mainRegion; });
    const auto peer = std::ranges::find_if(universe.regions(), [=](const Region& r) { return r.id == peerId; });
    if (main == universe.regions().end() || peer == universe.regions().end()) return std::nullopt;
    const bool occupied = std::ranges::any_of(universe.bodies(), [=](const Body& b) {
        return !b.stored && (b.region == peerId ||
            (b.portal_progress > 0.0 && b.portal_destination_region == peerId));
    });
    if (!occupied) return std::nullopt;
    const double width = std::min(680.0, main->width * 0.36);
    const double insetScale = std::min((width - 16.0) / peer->width,
                                      (main->height * 0.45 - 42.0) / peer->height);
    if (insetScale <= 0.0) return std::nullopt;
    PeerViewLayout layout;
    layout.region = *peer;
    layout.scale = insetScale;
    layout.contentSize = {peer->width * insetScale, peer->height * insetScale};
    layout.size = layout.contentSize + Vec2{16.0, 42.0};
    layout.origin = {main->x + main->width - layout.size.x - 12.0, main->y + 12.0};
    layout.contentOrigin = layout.origin + Vec2{8.0, 34.0};
    return layout;
}

struct CosmicRenderer::Impl {
    GLuint program = 0, vao = 0, buffer = 0;
    GLint projection = -1, resolution = -1, center = -1, size = -1;
    GLint angle = -1, stretch = -1, twist = -1, bend = -1, crop = -1;
    GLint mode = -1, color = -1, time = -1, radius = -1, phase = -1;
    GLint stars = -1, background = -1;
    GLint clip = -1;
    SP<Render::ITexture> peerLabel;
    std::size_t starCount = 180;
    float backgroundAlpha = 0.96F;
    GLsizei gridVertices = 0;
    bool attempted = false;
    std::string error;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();

    void quad(Vec2 position, Vec2 dimensions, int drawMode,
              std::array<float, 4> rgba, float rotation = 0, float elongation = 1,
              float winding = 0, float curvature = 0, bool mesh = false,
              float ringWidth = 0.03F, float ringPhase = 0) {
        glUniform2f(center, position.x, position.y);
        glUniform2f(size, dimensions.x, dimensions.y);
        glUniform1f(angle, rotation);
        glUniform1f(stretch, elongation);
        glUniform1f(twist, winding);
        glUniform1f(bend, curvature);
        glUniform1i(mode, drawMode);
        glUniform4fv(color, 1, rgba.data());
        glUniform1f(radius, ringWidth);
        glUniform1f(phase, ringPhase);
        glDrawArrays(GL_TRIANGLES, mesh ? 6 : 0, mesh ? gridVertices : 6);
    }
};

CosmicRenderer::CosmicRenderer() : m_impl(std::make_unique<Impl>()) {}
CosmicRenderer::~CosmicRenderer() = default;

bool CosmicRenderer::initialize() {
    auto& impl = *m_impl;
    if (impl.program) return true;
    if (impl.attempted) return false;
    impl.attempted = true;
    impl.error.clear();
    GLState state;
    GLuint vertex = 0, fragment = 0;
    try {
        vertex = compileShader(GL_VERTEX_SHADER, vertexShaderSource);
        fragment = compileShader(GL_FRAGMENT_SHADER, fragmentShaderSource);
        impl.program = glCreateProgram();
        if (!impl.program) throw std::runtime_error("Could not create Cosmic shader program");
        glAttachShader(impl.program, vertex);
        glAttachShader(impl.program, fragment);
        glLinkProgram(impl.program);
        GLint linked = GL_FALSE;
        glGetProgramiv(impl.program, GL_LINK_STATUS, &linked);
        if (!linked) {
            std::array<char, 2048> log{};
            glGetProgramInfoLog(impl.program, log.size(), nullptr, log.data());
            throw std::runtime_error(std::string("Cosmic shader link failed: ") + log.data());
        }
        glDeleteShader(vertex);
        glDeleteShader(fragment);
        vertex = fragment = 0;
        const auto uniform = [&](const char* name) { return glGetUniformLocation(impl.program, name); };
        impl.projection = uniform("uProjection"); impl.resolution = uniform("uResolution");
        impl.center = uniform("uCenter"); impl.size = uniform("uSize");
        impl.angle = uniform("uAngle"); impl.stretch = uniform("uStretch");
        impl.twist = uniform("uTwist"); impl.bend = uniform("uBend");
        impl.crop = uniform("uCrop"); impl.mode = uniform("uMode");
        impl.color = uniform("uColor"); impl.time = uniform("uTime");
        impl.radius = uniform("uRadius"); impl.phase = uniform("uPhase");
        impl.stars = uniform("uStars"); impl.background = uniform("uBackground");
        impl.clip = uniform("uClip");

        std::vector<float> vertices;
        const auto cell = [&](float left, float top, float right, float bottom) {
            vertices.insert(vertices.end(), {left, top, left, bottom, right, top,
                                            right, top, left, bottom, right, bottom});
        };
        cell(0, 0, 1, 1);
        constexpr int columns = 24, rows = 16;
        for (int y = 0; y < rows; ++y)
            for (int x = 0; x < columns; ++x)
                cell(float(x) / columns, float(y) / rows,
                     float(x + 1) / columns, float(y + 1) / rows);
        impl.gridVertices = columns * rows * 6;
        glGenVertexArrays(1, &impl.vao);
        glGenBuffers(1, &impl.buffer);
        glBindVertexArray(impl.vao);
        glBindBuffer(GL_ARRAY_BUFFER, impl.buffer);
        glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(float), vertices.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
        glUseProgram(impl.program);
        glUniform1i(uniform("uTexture"), 0);
        if (!impl.vao || !impl.buffer) throw std::runtime_error("Could not allocate Cosmic vertex mesh");
        impl.peerLabel = g_pHyprRenderer->renderText("OTHER UNIVERSE", CHyprColor{0.68, 0.84, 1.0, 1.0},
                                                   24, false, "sans-serif", 600, 600);
        return true;
    } catch (const std::exception& exception) {
        if (vertex) glDeleteShader(vertex);
        if (fragment) glDeleteShader(fragment);
        impl.error = exception.what();
        release();
        impl.attempted = true;
        return false;
    }
}

void CosmicRenderer::draw(PHLMONITOR monitor, const Universe& universe,
                          const std::vector<Snapshot>& snapshots, bool alternateRegion) {
    g_pHyprRenderer->addPassElement(pass(monitor, universe, snapshots, alternateRegion));
}

void CosmicRenderer::configure(std::size_t stars, double background) {
    m_impl->starCount = std::min<std::size_t>(stars, 4096);
    m_impl->backgroundAlpha = std::clamp(background, 0.0, 1.0);
}

void CosmicRenderer::executeDraw(PHLMONITOR monitor, const Universe& universe,
                                 const std::vector<Snapshot>& snapshots, bool alternateRegion) {
    if (!monitor || !initialize()) return;
    GLState state;
    auto& impl = *m_impl;
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(impl.program);
    glBindVertexArray(impl.vao);
    glActiveTexture(GL_TEXTURE0);
    const auto projection = g_pHyprRenderer->projectBoxToTarget(
        CBox{0, 0, monitor->m_transformedSize.x, monitor->m_transformedSize.y}).getMatrix();
    // getMatrix() is row-major; transpose on CPU for portable GLES semantics.
    std::array<float, 9> columnMajor{};
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column)
            columnMajor[column * 3 + row] = projection[row * 3 + column];
    glUniformMatrix3fv(impl.projection, 1, GL_FALSE, columnMajor.data());
    const double pixelScale = monitor->m_scale;
    const Vec2 monitorSize{monitor->m_transformedSize.x, monitor->m_transformedSize.y};
    glUniform2f(impl.resolution, monitorSize.x, monitorSize.y);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - impl.start).count();
    glUniform1f(impl.time, std::fmod(seconds, 10000.0));
    glUniform1f(impl.stars, impl.starCount);
    glUniform1f(impl.background, impl.backgroundAlpha);
    glUniform4f(impl.clip, -1, -1, monitorSize.x + 1, monitorSize.y + 1);
    glUniform4f(impl.crop, 0, 0, 1, 1);
    impl.quad(monitorSize * 0.5, monitorSize, 0, {1, 1, 1, 1});

    const Region* view = nullptr;
    for (const auto& region : universe.regions()) {
        const int wanted = static_cast<int>(monitor->m_id) + (alternateRegion ? 1000000 : 0);
        if (region.id == wanted) { view = &region; break; }
    }
    if (!view && !universe.regions().empty()) view = &universe.regions().front();
    if (!view) return;
    const auto toPixel = [&](Vec2 world, int region) {
        auto point = universe.worldToScreen(world, region);
        if (alternateRegion) return Vec2{(point.x - view->x) * monitorSize.x / view->width,
                                         (point.y - view->y) * monitorSize.y / view->height};
        return Vec2{(point.x - monitor->m_position.x) * pixelScale,
                    (point.y - monitor->m_position.y) * pixelScale};
    };
    const auto zoom = [&](int region) {
        for (const auto& camera : universe.cameras()) if (camera.region == region) return camera.zoom;
        return 1.0;
    };
    const double visualScale = alternateRegion ? monitorSize.x / view->width : pixelScale;
    const auto drawBody = [&](const Body& body, Vec2 position, double presentationScale) {
        if (body.stored || body.scale < 0.0001) return;
        const auto snapshot = std::ranges::find_if(snapshots, [&](const Snapshot& item) { return item.id == body.id; });
        if (snapshot == snapshots.end() || !snapshot->framebuffer || !snapshot->window) return;
        const auto texture = snapshot->framebuffer->getTexture();
        if (!texture || !texture->ok()) return;
        glBindTexture(GL_TEXTURE_2D, texture->m_texID);
        const auto& capture = *snapshot;
        glUniform4f(impl.crop,
            (capture.logicalBox.x - capture.monitorPosition.x) * capture.monitorScale / texture->m_size.x,
            (capture.logicalBox.y - capture.monitorPosition.y) * capture.monitorScale / texture->m_size.y,
            capture.logicalBox.w * capture.monitorScale / texture->m_size.x,
            capture.logicalBox.h * capture.monitorScale / texture->m_size.y);
        double deformation = body.sink_progress * 0.24;
        if (body.portal_progress > 0.0) {
            const double fraction = body.portal_entry_duration / (body.portal_entry_duration + body.portal_exit_duration);
            const double phase = body.portal_emerging
                ? 1.0 - (body.portal_progress - fraction) / (1.0 - fraction)
                : body.portal_progress / fraction;
            deformation = std::max(deformation, std::clamp(phase, 0.0, 1.0) * 0.16);
        }
        const double scale = presentationScale * body.scale;
        impl.quad(position, {body.width * scale, body.height * scale}, 1, {1, 1, 1, 1}, body.angle,
                  body.stretch, body.twist, universe.config().spaghetti ? deformation : 0.0, true);
    };

    // Natural mouths obey the same layering as a manually triggered hole:
    // only a luminous rim is allowed in front of intermediate imagery.
    if (universe.config().wormholes) {
        for (std::size_t i = 0; i < universe.wormholes().size(); ++i) {
            const auto& hole = universe.wormholes()[i];
            if (hole.region != view->id) continue;
            const auto palette = i % 2 ? std::array<float, 3>{1.0F, 0.47F, 0.25F}
                                       : std::array<float, 3>{0.35F, 0.68F, 1.0F};
            const double diameter = hole.radius * 2.7 * visualScale * zoom(hole.region);
            impl.quad(toPixel(hole.position, hole.region), {diameter, diameter}, 2,
                      {palette[0], palette[1], palette[2], 0.95F}, hole.angle, 1, 0, 0, false, 0.04F, 1);
        }
    }

    // Put the opaque event horizon behind the falling image. Drawing it last
    // hides a centered target before its shrinking/twisting can be seen.
    for (const auto& body : universe.bodies()) {
        if (body.region != view->id || body.sink_progress <= 0 || body.stored) continue;
        const auto position = toPixel(body.sink_center, body.region);
        const double diameter = (110 + 16 * std::sin(seconds * 2)) * visualScale;
        impl.quad(position, {diameter, diameter}, 2, {0.8F, 0.31F, 1.0F, 0.9F},
                  seconds * 0.6, 1, 0, 0, false, 0.035F, 1);
    }

    // Low-opacity velocity tails show orbital direction without cloning apps.
    for (const auto& body : universe.bodies()) {
        if (body.stored || body.region != view->id) continue;
        const auto position = toPixel(body.position, body.region);
        const auto palette = hue(double(body.id % 17) / 17.0);
        for (int trail = 1; trail <= 4; ++trail) {
            const auto tail = position - body.velocity * (trail * 0.025 * visualScale * zoom(body.region));
            const double dotSize = (7.0 - trail) * visualScale;
            impl.quad(tail, {dotSize * 2, dotSize * 2}, 3,
                      {palette[0], palette[1], palette[2], float(0.16 / trail)});
        }
    }
    for (const auto& body : universe.bodies()) {
        if (body.region != view->id) continue;
        drawBody(body, toPixel(body.position, body.region), visualScale * zoom(body.region));
    }
    glUniform4f(impl.crop, 0, 0, 1, 1);
    if (universe.config().wormholes) {
        for (std::size_t i = 0; i < universe.wormholes().size(); ++i) {
            const auto& hole = universe.wormholes()[i];
            if (hole.region != view->id) continue;
            const auto position = toPixel(hole.position, hole.region);
            const double diameter = hole.radius * 2.7 * visualScale * zoom(hole.region);
            const auto palette = i % 2 ? std::array<float, 3>{1.0F, 0.47F, 0.25F}
                                       : std::array<float, 3>{0.35F, 0.68F, 1.0F};
            impl.quad(position, {diameter, diameter}, 2,
                      {palette[0], palette[1], palette[2], 0.65F}, hole.angle, 1, 0, 0, false, 0.04F, 0);
        }
    }
    for (const auto& body : universe.bodies()) {
        if (body.region != view->id || body.sink_progress <= 0 || body.stored) continue;
        const auto position = toPixel(body.sink_center, body.region);
        const double diameter = (110 + 16 * std::sin(seconds * 2)) * visualScale;
        // The foreground accent is only the luminous rim, never a black disk.
        impl.quad(position, {diameter, diameter}, 2, {0.8F, 0.31F, 1.0F, 0.7F},
                  seconds * 0.6, 1, 0, 0, false, 0.035F, 0);
    }
    for (const auto& wave : universe.waves()) {
        if (wave.region != view->id) continue;
        const auto position = toPixel(wave.position, wave.region);
        const double diameter = wave.radius * 2.65 * visualScale * zoom(wave.region);
        const float alpha = std::clamp(wave.life / std::max(wave.lifetime, 0.001), 0.0, 1.0);
        impl.quad(position, {diameter, diameter}, 2, {1.0F, 0.65F, 0.32F, alpha},
                  0, 1, 0, 0, false, 0.012F, 0);
    }
    for (const auto& particle : universe.particles()) {
        if (particle.region != view->id) continue;
        const auto position = toPixel(particle.position, particle.region);
        const double diameter = particle.size * 4 * visualScale * zoom(particle.region);
        const float alpha = std::clamp(particle.life / std::max(particle.lifetime, 0.001), 0.0, 1.0);
        const auto palette = hue(particle.hue);
        impl.quad(position, {diameter, diameter}, 3, {palette[0], palette[1], palette[2], alpha});
    }
    if (universe.config().cursor_gravity && universe.gravityMode() == GravityMode::Cursor) {
        const auto cursor = g_pInputManager->getMouseCoordsInternal();
        impl.quad({(cursor.x - monitor->m_position.x) * pixelScale,
                   (cursor.y - monitor->m_position.y) * pixelScale},
                  {94 * pixelScale, 94 * pixelScale}, 2,
                  universe.rewinding() ? std::array<float, 4>{1.0F, 0.65F, 0.35F, 0.42F}
                                       : std::array<float, 4>{0.36F, 0.78F, 1.0F, 0.36F},
                  0, 1, 0, 0, false, 0.013F, 0);
    }

    // Keep the hidden counterpart visible for as long as it contains a living
    // body, not just during transfer/cooldown. F10 can promote it to the main view.
    if (const auto inset = peerViewLayout(universe, view->id)) {
        const auto panelPixel = [&](Vec2 point) { return (point - Vec2{monitor->m_position.x, monitor->m_position.y}) * pixelScale; };
        const auto origin = panelPixel(inset->origin);
        const auto size = inset->size * pixelScale;
        glUniform4f(impl.crop, 0, 0, 1, 1);
        impl.quad(origin + size * 0.5, size, 4, {0.19F, 0.34F, 0.53F, 0.96F});
        impl.quad(origin + size * 0.5, size - Vec2{2 * pixelScale, 2 * pixelScale}, 4,
                  {0.016F, 0.021F, 0.050F, 0.97F});
        if (impl.peerLabel && impl.peerLabel->ok()) {
            glBindTexture(GL_TEXTURE_2D, impl.peerLabel->m_texID);
            const double labelScale = std::min(0.5 * pixelScale, (size.x - 20 * pixelScale) / impl.peerLabel->m_size.x);
            const Vec2 labelSize{impl.peerLabel->m_size.x * labelScale, impl.peerLabel->m_size.y * labelScale};
            impl.quad(origin + Vec2{10 * pixelScale, 17 * pixelScale} + Vec2{labelSize.x * 0.5, 0}, labelSize,
                      1, {1, 1, 1, 1});
        }
        const auto content = panelPixel(inset->contentOrigin);
        const auto contentSize = inset->contentSize * pixelScale;
        glUniform4f(impl.clip, content.x, content.y, content.x + contentSize.x, content.y + contentSize.y);
        impl.quad(content + contentSize * 0.5, contentSize, 4, {0.008F, 0.010F, 0.026F, 1.0F});
        const auto peerPixel = [&](Vec2 world) {
            return panelPixel(inset->fromRegionScreen(universe.worldToScreen(world, inset->region.id)));
        };
        const double peerScale = inset->scale * pixelScale * zoom(inset->region.id);
        for (int foreground = 0; foreground < 2; ++foreground) {
            if (foreground) {
                for (const auto& body : universe.bodies())
                    if (body.region == inset->region.id) drawBody(body, peerPixel(body.position), peerScale);
                glUniform4f(impl.crop, 0, 0, 1, 1);
            }
            for (std::size_t i = 0; i < universe.wormholes().size(); ++i) {
                const auto& hole = universe.wormholes()[i];
                if (hole.region != inset->region.id) continue;
                const auto palette = i % 2 ? std::array<float, 3>{1.0F, 0.47F, 0.25F}
                                           : std::array<float, 3>{0.35F, 0.68F, 1.0F};
                const double diameter = hole.radius * 2.7 * peerScale;
                impl.quad(peerPixel(hole.position), {diameter, diameter}, 2,
                          {palette[0], palette[1], palette[2], foreground ? 0.65F : 0.95F},
                          hole.angle, 1, 0, 0, false, 0.04F, foreground ? 0 : 1);
            }
        }
        for (const auto& wave : universe.waves()) {
            if (wave.region != inset->region.id) continue;
            const double diameter = wave.radius * 2.65 * peerScale;
            const float alpha = std::clamp(wave.life / std::max(wave.lifetime, 0.001), 0.0, 1.0);
            impl.quad(peerPixel(wave.position), {diameter, diameter}, 2, {1.0F, 0.65F, 0.32F, alpha},
                      0, 1, 0, 0, false, 0.012F, 0);
        }
        for (const auto& particle : universe.particles()) {
            if (particle.region != inset->region.id) continue;
            const double diameter = particle.size * 4 * peerScale;
            const float alpha = std::clamp(particle.life / std::max(particle.lifetime, 0.001), 0.0, 1.0);
            const auto palette = hue(particle.hue);
            impl.quad(peerPixel(particle.position), {diameter, diameter}, 3, {palette[0], palette[1], palette[2], alpha});
        }
        glUniform4f(impl.clip, -1, -1, monitorSize.x + 1, monitorSize.y + 1);
    }
}

UP<IPassElement> CosmicRenderer::pass(PHLMONITOR monitor, const Universe& universe,
                                     const std::vector<Snapshot>& snapshots, bool alternateRegion) {
    return makeUnique<CosmicPass>(*this, monitor, universe, snapshots, alternateRegion);
}

void CosmicRenderer::release() {
    auto& impl = *m_impl;
    impl.peerLabel.reset();
    if (impl.buffer) glDeleteBuffers(1, &impl.buffer);
    if (impl.vao) glDeleteVertexArrays(1, &impl.vao);
    if (impl.program) glDeleteProgram(impl.program);
    impl.buffer = impl.vao = impl.program = 0;
    impl.attempted = false;
}

const std::string& CosmicRenderer::error() const { return m_impl->error; }

} // namespace cosmic
