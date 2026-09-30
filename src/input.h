#pragma once

#include <GLFW/glfw3.h>

struct CameraController; // forward declare from oglcamera.h

struct InputState {
    double mouseX = 0, mouseY = 0;
    double lastMouseX = 0, lastMouseY = 0;
    bool leftButton = false;
    bool rightButton = false;
    bool middleButton = false;
    bool ctrlHeld = false;
    bool shiftHeld = false;
    bool altHeld = false;
    double scrollDelta = 0;
    int windowWidth = 1280, windowHeight = 720;
};

class InputHandler {
public:
    void Init(GLFWwindow* window);
    void Update(CameraController* camera);
    InputState& GetState() { return m_state; }

    // GLFW callbacks (static, forwarded to instance)
    static void MouseButtonCallback(GLFWwindow* window, int button, int action, int mods);
    static void CursorPosCallback(GLFWwindow* window, double xpos, double ypos);
    static void ScrollCallback(GLFWwindow* window, double xoffset, double yoffset);
    static void KeyCallback(GLFWwindow* window, int key, int scancode, int action, int mods);
    static void FramebufferSizeCallback(GLFWwindow* window, int width, int height);

private:
    InputState m_state;
    static InputHandler* s_instance;
};
