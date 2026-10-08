#pragma once

#include "physics.hpp"
#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/render/Framebuffer.hpp>
#include <hyprland/src/render/pass/PassElement.hpp>
#include <memory>
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

class CosmicRenderer {
  public:
    CosmicRenderer();
    ~CosmicRenderer();
    CosmicRenderer(const CosmicRenderer&) = delete;
    CosmicRenderer& operator=(const CosmicRenderer&) = delete;

    // Must run with the compositor GL context current. draw() initializes lazily.
    bool initialize();
    void draw(PHLMONITOR monitor, const Universe& universe,
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
