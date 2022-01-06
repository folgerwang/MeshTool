#include "coremath.h"
#include "coregeographic.h"
#include "oglcamera.h"

CameraController::CameraController()
{
    start_dist_from_focus_point = 0;
    end_dist_beyond_focus_point = 0;
}

void CameraController::Reset()
{
    start_dist_from_focus_point = 0;
    end_dist_beyond_focus_point = 0;

    cur_input.Reset();
    last_input = cur_input;
    cur_camera_info.Reset();
    last_camera_info = cur_camera_info;
}

void CameraController::InitPlanar(const core::bounds3d& bbox_ws, float tan_fov_y, float aspect_x)
{
    cur_input.Reset();
    last_input.Reset();

    core::vec3d diagonal = bbox_ws.GetDiagonal() * 0.5;
    float width_y = max(diagonal.y, diagonal.x / aspect_x);
    start_dist_from_focus_point = width_y / tan_fov_y + diagonal.z;
    cur_camera_info.dist_to_focus_point = start_dist_from_focus_point;
    end_dist_beyond_focus_point = -diagonal.z;
    cur_camera_info.focus_point = bbox_ws.GetCentroid();
    last_camera_info = cur_camera_info;
}

void CameraController::InitSphere(float tan_fov_y)
{
    cur_input.Reset();
    last_input.Reset();

    double radius = core::emajor;
    double r2 = radius * radius;
    double x = radius / double(tan_fov_y) * 1.1;
    start_dist_from_focus_point = float(sqrt(x * x + r2));
    end_dist_beyond_focus_point = float(r2 / double(start_dist_from_focus_point));

    cur_camera_info.dist_to_focus_point = start_dist_from_focus_point;
    cur_camera_info.focus_point = core::vec3f(0, 0, 0);
    last_camera_info = cur_camera_info;
}

core::vec2f GetScreenPositionByCursor(core::vec2i cursor_pos, float tan_fov_y, float aspect_x, int buffer_width, int buffer_height)
{
    return core::vec2f(float(cursor_pos.x) / buffer_width * 2.0f - 1.0f,
                       float(cursor_pos.y) / buffer_height * 2.0f - 1.0f) *
                       core::vec2f(aspect_x, -1.0f) * tan_fov_y;
}

void CameraController::UpdateOnSphere(float tan_fov_y, float aspect_x, int buffer_width, int buffer_height, bool force/* = false*/)
{
    if (force || cur_input.cursor_pos != last_input.cursor_pos || cur_input.wheel_angle != last_input.wheel_angle)
    {
        float log_range_z = log2(1.0f + start_dist_from_focus_point - end_dist_beyond_focus_point);
        core::vec2f last_screen_uv = GetScreenPositionByCursor(last_input.cursor_pos, tan_fov_y, aspect_x, buffer_width, buffer_height);
        core::vec2f screen_uv = GetScreenPositionByCursor(cur_input.cursor_pos, tan_fov_y, aspect_x, buffer_width, buffer_height);
        auto screen_uv_diff = last_screen_uv - screen_uv;

        if (cur_input.wheel_angle != last_input.wheel_angle)
        {
            cur_camera_info.dist_to_focus_point = (exp2((1.0f - max(min(cur_input.wheel_angle, 300), 0) / 300.0f) * log_range_z) - 1.0f) + end_dist_beyond_focus_point;
            cur_camera_info.focus_point = last_camera_info.focus_point - core::vec3d(screen_uv.x, screen_uv.y, 0.0) * GetCameraDistanceDiff();
            last_camera_info = cur_camera_info;
        }
        else if (cur_input.cursor_pos != last_input.cursor_pos)
        {
            cur_camera_info.focus_point = last_camera_info.focus_point + core::vec3d(screen_uv_diff.x, screen_uv_diff.y, 0.0) * last_camera_info.dist_to_focus_point;
            last_camera_info = cur_camera_info;
        }

        last_input = cur_input;
    }
}

void CameraController::UpdateOnPlanar(float tan_fov_y, float aspect_x, int buffer_width, int buffer_height, bool force/* = false*/)
{
    if (force || cur_input.cursor_pos != last_input.cursor_pos || cur_input.wheel_angle != last_input.wheel_angle)
    {
        float log_range_z = log2(1.0f + start_dist_from_focus_point - end_dist_beyond_focus_point);
        core::vec2f last_screen_uv = GetScreenPositionByCursor(last_input.cursor_pos, tan_fov_y, aspect_x, buffer_width, buffer_height);
        core::vec2f screen_uv = GetScreenPositionByCursor(cur_input.cursor_pos, tan_fov_y, aspect_x, buffer_width, buffer_height);
        auto screen_uv_diff = last_screen_uv - screen_uv;

        if (cur_input.wheel_angle != last_input.wheel_angle)
        {
            cur_camera_info.dist_to_focus_point = (exp2((1.0f - max(min(cur_input.wheel_angle, 300), 0) / 300.0f) * log_range_z) - 1.0f) + end_dist_beyond_focus_point;
            cur_camera_info.focus_point = last_camera_info.focus_point - core::vec3d(screen_uv.x, screen_uv.y, 0.0) * GetCameraDistanceDiff();
            last_camera_info = cur_camera_info;
        }
        else if (cur_input.cursor_pos != last_input.cursor_pos)
        {
            cur_camera_info.focus_point = last_camera_info.focus_point + core::vec3d(screen_uv_diff.x, screen_uv_diff.y, 0.0) * last_camera_info.dist_to_focus_point;
            last_camera_info = cur_camera_info;
        }

        last_input = cur_input;
    }
}
