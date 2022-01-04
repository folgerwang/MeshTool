#pragma once

#include "coremath.h"

struct InputInfo
{
    core::vec2i   cursor_pos;
    int           wheel_angle;
    bool          alt_pressed;
    bool          ctrl_pressed;
    bool          shft_pressed;

    InputInfo()
    {
        Reset();
    }

    bool function_key_pressed()
    {
        return alt_pressed || ctrl_pressed || shft_pressed;
    }

    void Reset()
    {
        cursor_pos.x = 0;
        cursor_pos.y = 0;
        wheel_angle = 0;
        alt_pressed = false;
        ctrl_pressed = false;
        shft_pressed = false;
    }
};

struct CameraInfo
{
    core::vec3f focus_point;
    float       dist_to_focus_point;

    CameraInfo()
    {
        Reset();
    }

    void Reset()
    {
        focus_point = core::vec3f(0, 0, 0);
        dist_to_focus_point = 0;
    }
};

struct CameraController
{
    enum UpdateLastPostionType
    {
        kNoUpdate,
        kCopyBeforeUpdate,
        kCopyAfterUpdate
    };

    InputInfo   cur_input;
    InputInfo   last_input;

    CameraInfo  cur_camera_info;
    CameraInfo  last_camera_info;

    float       start_dist_from_focus_point;
    float       end_dist_beyond_focus_point;

    CameraController();

    void InitPlanar(const core::bounds3f& bbox_ws, float tan_fov_y, float aspect_x);
    void InitSphere(float tan_fov_y);
    void UpdateOnSphere(float tan_fov_y, float aspect_x, int buffer_width, int buffer_height, bool force = false);
    void UpdateOnPlanar(float tan_fov_y, float aspect_x, int buffer_width, int buffer_height, bool force = false);
    float GetCameraDistanceDiff() { return cur_camera_info.dist_to_focus_point - last_camera_info.dist_to_focus_point; }
    void UpdateCursorPos(int new_x, int new_y,  UpdateLastPostionType type)
    {
        if (type == kCopyBeforeUpdate)
        {
            last_input.cursor_pos = cur_input.cursor_pos;
        }
        cur_input.cursor_pos = core::vec2i(new_x, new_y);
        if (type == kCopyAfterUpdate)
        {
            last_input.cursor_pos = cur_input.cursor_pos;
        }
    }
    void UpdateWheelAngle(int angle_step)
    {
        last_input.wheel_angle = cur_input.wheel_angle;
        cur_input.wheel_angle += angle_step;
        cur_input.wheel_angle = cur_input.wheel_angle > 0 ? (cur_input.wheel_angle < 300 ? cur_input.wheel_angle : 300) : 0;
    }

    void UpdateKeyInfo()
    {
        last_input.alt_pressed = cur_input.alt_pressed;
        last_input.ctrl_pressed = cur_input.ctrl_pressed;
        last_input.shft_pressed = cur_input.shft_pressed;
    }
    void Reset();
};
