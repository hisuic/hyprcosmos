#pragma once

#include "physics.hpp"
#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/render/Framebuffer.hpp>
#include <hyprland/src/render/pass/PassElement.hpp>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cosmic {

// A monitor-sized compositor capture. The crop stays in the normal desktop's
// absolute logical coordinates; no application position or size is modified.
struct Snapshot {
    uint64_t id = 0;
    PHLWINDOWREF window;
    SP<Render::IFramebuffer> framebuffer;
    CBox logicalBox;
    Vector2D monitorPosition;
    float monitorScale = 1.0F;
};

// Absolute logical coordinates shared by presentation and dedicated controls.
// Ordinary pointer input still restores Hyprland's normal layout first.
struct PeerViewLayout {
    Region region;
    Vec2 origin;
    Vec2 size;
    Vec2 contentOrigin;
    Vec2 contentSize;
    double scale = 1.0;
    bool contains(Vec2 point) const;
    bool containsContent(Vec2 point) const;
    Vec2 toRegionScreen(Vec2 point) const;
    Vec2 fromRegionScreen(Vec2 point) const;
};
std::optional<PeerViewLayout> peerViewLayout(const Universe& universe, int mainRegion);

class CosmicRenderer {
  public:
    CosmicRenderer();
    ~CosmicRenderer();
    CosmicRenderer(const CosmicRenderer&) = delete;
    CosmicRenderer& operator=(const CosmicRenderer&) = delete;

    // Must run with the compositor GL context current. draw() initializes lazily.
    bool initialize();
    // Start the ambient clock at each idle entry; no background work while idle
    // detection is waiting in the normal desktop.
    void beginScene();
    void configure(std::size_t stars, double background);
    // Enqueues a custom pass; OpenGL drawing occurs when Hyprland executes it.
    void draw(PHLMONITOR monitor, const Universe& universe,
              const std::vector<Snapshot>& snapshots, bool alternateRegion = false);
    void executeDraw(PHLMONITOR monitor, const Universe& universe,
                     const std::vector<Snapshot>& snapshots, bool alternateRegion = false);
    // The returned pass runs within the current compositor frame. universe and
    // this renderer must outlive that frame; captures hold their own references.
    UP<IPassElement> pass(PHLMONITOR monitor, const Universe& universe,
                         const std::vector<Snapshot>& snapshots, bool alternateRegion = false);
    void release();
    const std::string& error() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace cosmic
