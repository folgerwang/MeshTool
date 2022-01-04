#pragma once

#include "coremath.h"
#include "coretexture.h"
#include "oglshaders.h"
#include "oglcamera.h"
#include "glfunctionlist.h"
#include <QWidget>
#include <QPushButton>
#include <QMouseEvent>
#include <QOpenGLWidget>
#include <gl/GLU.h>
#include <gl/GL.h>
#include <QOpenGLFunctions_4_3_Core>

struct MeshData;
struct WorldData;
struct BatchMeshData;

bool CullingCore(const core::bounds3f& bbox, const core::matrix4f& world_proj_mat);
void LoadTextureFromFile(const string& file_name, core::Texture2DInfo* tex_info);

class OGLWidget : public QOpenGLWidget, protected QOpenGLFunctions_4_3_Core
{
    Q_OBJECT
public:
    explicit OGLWidget(QWidget *parent = nullptr);

    void mouseMoveEvent(QMouseEvent * event) override;
    void mousePressEvent(QMouseEvent * event) override;
    void mouseReleaseEvent(QMouseEvent * event)override;
    void wheelEvent(QWheelEvent *event) override;
    void scrollWithPixels(QWheelEvent *event, QPoint num_pixels);
    void scrollWithDegrees(QWheelEvent *event, QPoint num_degrees);

signals:

public slots:

protected:
    void initializeGL() Q_DECL_OVERRIDE;
    void resizeGL(int w, int h) Q_DECL_OVERRIDE;
    void paintGL() Q_DECL_OVERRIDE;
    bool checkCompileError(GLuint shader, string shader_body, bool is_strict = false);
    GLuint loadShader(const string& shader_body, const vector<string>& headers, ShaderType shader_type);
    GLuint createProgram(GLuint vertex_shader, GLuint pixel_shader);
    GLuint createProgram(GLuint compute_shader);

    void checkGLError(const char* file, int32_t line);
    GLuint getAttribLocation(GLuint program, string attrib);
    GLuint getUniformLocation(GLuint program, string uniform);
    void bindTexture2D(GLuint program, string uniform, int32_t unit, GLuint tex);
    void bindTexture2D(GLint index, int32_t unit, GLuint tex);
    void bindTexture2DMultisample(GLuint program, string uniform, int32_t unit, GLuint tex);
    void bindTexture2DMultisample(GLint index, int32_t unit, GLuint tex);
    void bindTextureRect(GLuint program, string uniform, int32_t unit, GLuint tex);
    void bindTextureRect(GLint index, int32_t unit, GLuint tex);
    void bindTextureArray(GLuint program, string uniform, int32_t unit, GLuint tex);
    void bindTextureArray(GLint index, int32_t unit, GLuint tex);
    void setUniform1i(GLuint program, string uniform, int32_t value);
    void setUniform1i(GLint index, int32_t value);
    void setUniform2i(GLuint program, string uniform, int32_t x, int32_t y);
    void setUniform2i(GLint index, int32_t x, int32_t y);
    void setUniform3i(GLuint program, string uniform, int32_t x, int32_t y, int32_t z);
    void setUniform3i(GLint index, int32_t x, int32_t y, int32_t z);
    void setUniform1f(GLuint program, string uniform, float value);
    void setUniform1f(GLint index, float value);
    void setUniform2f(GLuint program, string uniform, float x, float y);
    void setUniform2f(GLint index, float x, float y);
    void setUniform3f(GLuint program, string uniform, float x, float y, float z);
    void setUniform3f(GLint index, float x, float y, float z);
    void setUniform4f(GLuint program, string uniform, float x, float y, float z, float w);
    void setUniform4f(GLint index, float x, float y, float z, float w);
    void setUniform3fv(GLuint program, string uniform, const float *value, int32_t count);
    void setUniform3fv(GLint index, const float *value, int32_t count);
    void setUniform4fv(GLuint program, string uniform, const float *value, int32_t count);
    void setUniform4fv(GLint index, const float *value, int32_t count);
    void setUniformMatrix4fv(GLuint program, string uniform, const float *m, int32_t count, bool transpose);
    void setUniformMatrix4fv(GLint index, const float *m, int32_t count, bool transpose);

    bool initShaders();

    GLuint LoadTexture(const core::Texture2DInfo* tex_info);
    GLuint LoadTexture(const QImage* tex_info);
    void UploadTextures(BatchMeshData* batch_meshes);
    void DrawBatchMeshes(BatchMeshData* batch_meshes, const core::matrix4f& world_screen_mat, GLuint program, float scale, bool draw_tri_meshes);
    void DrawBatchMeshesSelectMask(BatchMeshData* batch_meshes, const core::matrix4f& world_screen_mat, GLuint program, float scale);
    void DrawWorldMeshes(WorldData* world, const core::matrix4f& world_screen_mat, GLuint program, float scale, bool draw_tri_meshes);
    void DrawWorldMeshesSelectMask(WorldData* world, const core::matrix4f& world_screen_mat, GLuint program, float scale);
    void Draw(const MeshData* mesh_data, uint32_t draw_call_index, GLuint program, float scale);
    void DrawMask(const MeshData* mesh_data, uint32_t draw_call_index, GLuint program, float scale);
    void DrawQuad(const core::vec4f& quad_params, uint32_t tex_id);
    void DrawReferencePosition(const core::matrix4f& world_screen_mat, float scale);
    void SelectPatches(const core::matrix4f& world_screen_mat, const float& scale);

private:
    CameraController camera_ctrl;

    core::bounds3f last_bbox_ws;

    int bufferWidth, bufferHeight;
    int mousePressed = 0;

    QPushButton* regionSelectButton;

    GLuint m_programs[kNumPrograms];
    core::matrix4f m_proj_mat;
    core::matrix4f m_view_mat;
    unique_ptr<core::vec2f[]> m_quad_vert;
    unique_ptr<uint16_t[]> m_quad_index;
    GLuint m_star_tex_idx;
    vector<core::vec2i> select_click_list;
    vector<core::vec2i> select_move_list;
    vector<core::vec2i> select_remove_list;

#ifndef CHECK_GL_ERROR
#define CHECK_GL_ERROR() checkGLError(__FILE__, __LINE__)
#endif
};

