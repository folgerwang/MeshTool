#include "input.h"
#include "oglcamera.h"

InputHandler* InputHandler::s_instance = nullptr;

void InputHandler::Init(GLFWwindow* window)
{
    s_instance = this;

    glfwSetMouseButtonCallback(window, MouseButtonCallback);
    glfwSetCursorPosCallback(window, CursorPosCallback);
    glfwSetScrollCallback(window, ScrollCallback);
    glfwSetKeyCallback(window, KeyCallback);
    glfwSetFramebufferSizeCallback(window, FramebufferSizeCallback);

    // Initialize window size from the actual framebuffer
    glfwGetFramebufferSize(window, &m_state.windowWidth, &m_state.windowHeight);

    // Initialize cursor position
    glfwGetCursorPos(window, &m_state.mouseX, &m_state.mouseY);
    m_state.lastMouseX = m_state.mouseX;
    m_state.lastMouseY = m_state.mouseY;
}

void InputHandler::Update(CameraController* camera)
{
    if (!camera)
        return;

    // Update modifier key state on the camera
    camera->cur_input.alt_pressed = m_state.altHeld;
    camera->cur_input.ctrl_pressed = m_state.ctrlHeld;
    camera->cur_input.shft_pressed = m_state.shiftHeld;
    camera->UpdateKeyInfo();

    // Update cursor position when left button is held (orbit/pan)
    if (m_state.leftButton)
    {
        CameraController::UpdateLastPostionType updateType =
            (m_state.lastMouseX == m_state.mouseX && m_state.lastMouseY == m_state.mouseY)
            ? CameraController::kNoUpdate
            : CameraController::kCopyBeforeUpdate;

        camera->UpdateCursorPos(
            static_cast<int>(m_state.mouseX),
            static_cast<int>(m_state.mouseY),
            updateType);
    }
    else
    {
        // When button released, sync last position so next press starts clean
        camera->UpdateCursorPos(
            static_cast<int>(m_state.mouseX),
            static_cast<int>(m_state.mouseY),
            CameraController::kCopyAfterUpdate);
    }

    // Handle scroll wheel -> camera zoom
    if (m_state.scrollDelta != 0.0)
    {
        int steps = static_cast<int>(m_state.scrollDelta);
        camera->UpdateWheelAngle(steps);
        m_state.scrollDelta = 0.0;
    }

    // Store last mouse position for next frame
    m_state.lastMouseX = m_state.mouseX;
    m_state.lastMouseY = m_state.mouseY;
}

// ---------------------------------------------------------------------------
// GLFW callbacks
// ---------------------------------------------------------------------------

void InputHandler::MouseButtonCallback(GLFWwindow* /*window*/, int button, int action, int /*mods*/)
{
    if (!s_instance) return;

    bool pressed = (action == GLFW_PRESS);

    switch (button)
    {
    case GLFW_MOUSE_BUTTON_LEFT:   s_instance->m_state.leftButton   = pressed; break;
    case GLFW_MOUSE_BUTTON_RIGHT:  s_instance->m_state.rightButton  = pressed; break;
    case GLFW_MOUSE_BUTTON_MIDDLE: s_instance->m_state.middleButton = pressed; break;
    default: break;
    }
}

void InputHandler::CursorPosCallback(GLFWwindow* /*window*/, double xpos, double ypos)
{
    if (!s_instance) return;

    s_instance->m_state.mouseX = xpos;
    s_instance->m_state.mouseY = ypos;
}

void InputHandler::ScrollCallback(GLFWwindow* /*window*/, double /*xoffset*/, double yoffset)
{
    if (!s_instance) return;

    // Accumulate scroll; positive yoffset = scroll up = zoom in
    s_instance->m_state.scrollDelta += yoffset * 5.0; // scale for camera sensitivity
}

void InputHandler::KeyCallback(GLFWwindow* /*window*/, int key, int /*scancode*/, int action, int /*mods*/)
{
    if (!s_instance) return;

    bool pressed = (action == GLFW_PRESS || action == GLFW_REPEAT);

    switch (key)
    {
    case GLFW_KEY_LEFT_CONTROL:
    case GLFW_KEY_RIGHT_CONTROL:
        s_instance->m_state.ctrlHeld = pressed;
        break;
    case GLFW_KEY_LEFT_SHIFT:
    case GLFW_KEY_RIGHT_SHIFT:
        s_instance->m_state.shiftHeld = pressed;
        break;
    case GLFW_KEY_LEFT_ALT:
    case GLFW_KEY_RIGHT_ALT:
        s_instance->m_state.altHeld = pressed;
        break;
    default:
        break;
    }
}

void InputHandler::FramebufferSizeCallback(GLFWwindow* /*window*/, int width, int height)
{
    if (!s_instance) return;

    s_instance->m_state.windowWidth = width;
    s_instance->m_state.windowHeight = height;
}
