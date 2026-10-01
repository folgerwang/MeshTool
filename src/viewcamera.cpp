#include "viewcamera.h"

#include <cmath>
#include <algorithm>
#include <GLFW/glfw3.h>

namespace
{
    const double kPi = 3.14159265358979323846;
    const double kMaxPitch = kPi * 0.5 - 0.01;
    const double kOrbitSpeed = 0.005;   // radians per pixel
    const double kLookSpeed = 0.0035;
    const double kDollySpeed = 0.004;   // log-distance per pixel
    const double kWheelZoom = 0.85;     // distance factor per wheel notch
    const double kMinDistance = 0.05;

    bool KeyDown(GLFWwindow* w, int key) { return glfwGetKey(w, key) == GLFW_PRESS; }
    bool ButtonDown(GLFWwindow* w, int b) { return glfwGetMouseButton(w, b) == GLFW_PRESS; }
}

core::vec3d ViewCamera::Forward() const
{
    double cp = cos(pitch);
    return core::vec3d(sin(yaw) * cp, cos(yaw) * cp, sin(pitch));
}

core::vec3d ViewCamera::Right() const
{
    return core::vec3d(cos(yaw), -sin(yaw), 0.0);   // normalize(cross(Forward(), +Z))
}

core::vec3d ViewCamera::Up() const
{
    return cross(Right(), Forward());
}

void ViewCamera::Frame(const core::bounds3d& bbox, float aspect)
{
    if (!bbox.b_valid)
        return;
    target = bbox.GetCentroid();
    double radius = (std::max)(length(bbox.GetDiagonal()) * 0.5, 0.5);
    sceneRadius = radius;
    // Fit the bounding sphere in the narrower of the two fields of view.
    double halfFov = fovY * 0.5;
    if (aspect < 1.0f)
        halfFov = atan(tan(halfFov) * aspect);
    distance = radius / sin(halfFov) * 1.05;
}

void ViewCamera::Pan(double dx, double dy, float viewportH)
{
    // Move the pivot so the point under the cursor follows it.
    double unitsPerPixel = 2.0 * distance * tan(fovY * 0.5) / (std::max)(viewportH, 1.0f);
    target = target - Right() * (dx * unitsPerPixel) + Up() * (dy * unitsPerPixel);
}

void ViewCamera::Update(GLFWwindow* window, double& scrollDelta, const Rect& vp, bool uiWantsMouse, double dt)
{
    double mx, my;
    glfwGetCursorPos(window, &mx, &my);
    double dx = m_haveLast ? mx - m_lastX : 0.0;
    double dy = m_haveLast ? my - m_lastY : 0.0;
    m_lastX = mx; m_lastY = my; m_haveLast = true;

    const bool lmb = ButtonDown(window, GLFW_MOUSE_BUTTON_LEFT);
    const bool mmb = ButtonDown(window, GLFW_MOUSE_BUTTON_MIDDLE);
    const bool rmb = ButtonDown(window, GLFW_MOUSE_BUTTON_RIGHT);
    const bool alt = KeyDown(window, GLFW_KEY_LEFT_ALT) || KeyDown(window, GLFW_KEY_RIGHT_ALT);
    const bool shift = KeyDown(window, GLFW_KEY_LEFT_SHIFT) || KeyDown(window, GLFW_KEY_RIGHT_SHIFT);
    const bool overViewport = !uiWantsMouse &&
        mx >= vp.x && mx < vp.x + vp.w && my >= vp.y && my < vp.y + vp.h;

    // Pick the drag mode from the current button/modifier combination. A drag
    // only starts over the viewport, but continues wherever the cursor goes.
    DragMode mode = kNone;
    if (alt)
    {
        if (lmb && !mmb && !rmb) mode = kOrbit;
        else if (mmb || (lmb && rmb)) mode = kPan;
        else if (rmb) mode = kDolly;
    }
    else
    {
        if (lmb && rmb) mode = kPan;
        else if (rmb) mode = kLook;
        else if (mmb) mode = kPan;
        else if (lmb) mode = kWalk;
    }
    const bool anyButton = lmb || mmb || rmb;
    if (!anyButton)
        m_blocked = false;
    else if (m_drag == kNone && !overViewport)
        m_blocked = true;   // press began on UI or outside the viewport: ignore until release
    if (m_blocked || !anyButton)
        mode = kNone;
    if (m_drag == kNone && mode != kNone)
        dx = dy = 0.0;      // first frame of a drag: no jump

    switch (mode)
    {
    case kOrbit:
        yaw += dx * kOrbitSpeed;
        pitch = std::clamp(pitch - dy * kOrbitSpeed, -kMaxPitch, kMaxPitch);
        break;
    case kPan:
        Pan(dx, dy, vp.h);
        break;
    case kDolly:
        distance = (std::max)(distance * exp(-(dx + dy) * kDollySpeed), kMinDistance);
        break;
    case kLook:
    {
        core::vec3d eye = Eye();
        yaw += dx * kLookSpeed;
        pitch = std::clamp(pitch - dy * kLookSpeed, -kMaxPitch, kMaxPitch);
        target = eye + Forward() * distance;
        break;
    }
    case kWalk:
    {
        // Unreal LMB: mouse X turns, mouse Y moves along the ground.
        core::vec3d eye = Eye();
        yaw += dx * kLookSpeed;
        core::vec3d flat = core::vec3d(sin(yaw), cos(yaw), 0.0);
        double step = (std::max)(distance, 1.0) * 0.004;
        eye = eye - flat * (dy * step);
        target = eye + Forward() * distance;
        break;
    }
    default:
        break;
    }
    m_drag = mode;

    // Wheel: fly speed while looking (Unreal), otherwise zoom toward the pivot.
    if (scrollDelta != 0.0)
    {
        double notches = scrollDelta / 5.0;   // InputHandler scales each notch by 5
        if (mode == kLook)
            flySpeedScale = std::clamp(flySpeedScale * pow(1.25, notches), 0.01, 100.0);
        else if (overViewport)
            distance = (std::max)(distance * pow(kWheelZoom, notches), kMinDistance);
        scrollDelta = 0.0;
    }

    // Unreal fly keys, only while looking with RMB.
    if (mode == kLook)
    {
        core::vec3d move(0.0, 0.0, 0.0);
        if (KeyDown(window, GLFW_KEY_W)) move = move + Forward();
        if (KeyDown(window, GLFW_KEY_S)) move = move - Forward();
        if (KeyDown(window, GLFW_KEY_D)) move = move + Right();
        if (KeyDown(window, GLFW_KEY_A)) move = move - Right();
        if (KeyDown(window, GLFW_KEY_E)) move = move + core::vec3d(0.0, 0.0, 1.0);
        if (KeyDown(window, GLFW_KEY_Q)) move = move - core::vec3d(0.0, 0.0, 1.0);
        if (length(move) > 0.0)
        {
            double speed = (std::max)(distance, 1.0) * 0.6 * flySpeedScale * (shift ? 4.0 : 1.0);
            target = target + normalize(move) * (speed * dt);
        }
    }
}

void ViewCamera::BuildViewProj(float aspect, float out[16]) const
{
    core::vec3d f = Forward(), r = Right(), u = Up();

    // View: rotation only (eye at the origin), column-major, camera looks down -Z.
    double V[16] = {
        r.x, u.x, -f.x, 0.0,
        r.y, u.y, -f.y, 0.0,
        r.z, u.z, -f.z, 0.0,
        0.0, 0.0,  0.0, 1.0,
    };

    // Projection: right-handed, Vulkan clip (Y down, depth 0..1).
    double nearZ = (std::max)(distance * 0.002, 0.02);
    double farZ = (distance + sceneRadius * 2.0) * 10.0;
    double t = 1.0 / tan(fovY * 0.5);
    double P[16] = {
        t / aspect, 0.0, 0.0,                              0.0,
        0.0,        -t,  0.0,                              0.0,
        0.0,        0.0, farZ / (nearZ - farZ),           -1.0,
        0.0,        0.0, nearZ * farZ / (nearZ - farZ),    0.0,
    };

    for (int c = 0; c < 4; c++)
        for (int rr = 0; rr < 4; rr++)
        {
            double s = 0.0;
            for (int k = 0; k < 4; k++)
                s += P[k * 4 + rr] * V[c * 4 + k];
            out[c * 4 + rr] = float(s);
        }
}
