#pragma once

#include "coremath.h"

struct GLFWwindow;

// 3D viewport camera with Maya- and Unreal-style navigation, Z-up world.
//
//   Maya (hold Alt)                 Unreal
//   Alt+LMB  tumble around pivot    RMB drag       look around (turn in place)
//   Alt+MMB  pan                    RMB + WASD/QE  fly (wheel = speed, Shift = fast)
//   Alt+RMB  dolly                  LMB drag       move forward/back + turn
//   Wheel    zoom to pivot          MMB / LMB+RMB  pan
//
// The camera orbits a pivot (target) at `distance`; fly/look moves the eye and
// drags the pivot along so Alt-orbiting afterwards stays intuitive.
class ViewCamera
{
public:
    // Viewport rectangle in window (cursor) coordinates.
    struct Rect { float x = 0, y = 0, w = 1, h = 1; };

    core::vec3d target = core::vec3d(0.0, 0.0, 0.0);
    double      yaw = 0.0;          // radians around +Z; 0 looks along +Y
    double      pitch = -0.6;       // radians; negative looks down
    double      distance = 10.0;    // eye to pivot
    double      sceneRadius = 10.0; // for the far plane
    double      flySpeedScale = 1.0;
    float       fovY = 3.14159265f / 4.0f;
    // Orthographic projection: the view spans what the perspective view
    // would at the pivot (half height = distance * tan(fovY / 2)), so zoom,
    // pan and framing keep working unchanged.
    bool        orthographic = false;
    double      OrthoHalfHeight() const { return distance * tan(fovY * 0.5); }
    // Straight down, north up: the bird's-eye view used for top-down screenshots.
    void        LookStraightDown();

    // Point the camera at a bounding box, keeping the current view direction.
    void Frame(const core::bounds3d& bbox, float aspect);

    // Polls mouse/keyboard. `uiWantsMouse` is ImGui's WantCaptureMouse: drags
    // and wheel only start when the cursor is in `viewport` and over no UI.
    void Update(GLFWwindow* window, double& scrollDelta, const Rect& viewport, bool uiWantsMouse, double dt);

    core::vec3d Forward() const;
    core::vec3d Right() const;
    core::vec3d Up() const;
    core::vec3d Eye() const { return target - Forward() * distance; }

    // Column-major view-projection for camera-relative positions (eye at the
    // origin), Vulkan clip space (Y down, depth 0..1).
    void BuildViewProj(float aspect, float outViewProj[16]) const;
    // The projection's near and far planes (metres from the eye).
    double NearZ() const { return (std::max)(distance * 0.002, 0.02); }
    double FarZ() const { return (distance + sceneRadius * 2.0) * 10.0; }

private:
    enum DragMode { kNone, kOrbit, kPan, kDolly, kLook, kWalk };
    DragMode m_drag = kNone;
    double   m_lastX = 0.0, m_lastY = 0.0;
    bool     m_haveLast = false;
    bool     m_blocked = false;     // buttons went down off-viewport; wait for release

    void Pan(double dx, double dy, float viewportH);
};
