#include "oglshaders.h"
#include "oglwidget.h"
#include "meshdata.h"
#include "debugout.h"
#include "glfunctionlist.h"
#include <QLoggingCategory>

void OGLWidget::Draw(const MeshData* mesh_data, uint32_t draw_call_index, GLuint program, float scale)
{
    if (mesh_data && draw_call_index < mesh_data->draw_call_list.size())
    {
        const DrawCallInfo& draw_call_info = mesh_data->draw_call_list[draw_call_index];
        if (draw_call_info.is_drawable())
        {
            GLuint pos_attrib_idx = getAttribLocation(program, "aPosition");
            if (mesh_data->vertex_list && pos_attrib_idx != GLuint(-1))
            {
                glVertexAttribPointer(GLuint(pos_attrib_idx), 3, GL_FLOAT, false, 3 * sizeof(float), mesh_data->vertex_list.get());
                glEnableVertexAttribArray(GLuint(pos_attrib_idx));
            }

            GLuint uv_attrib_idx = getAttribLocation(program, "aTexcoord");
            if (mesh_data->uv_list && uv_attrib_idx != GLuint(-1))
            {
                glVertexAttribPointer(uv_attrib_idx, 2, GL_FLOAT, false, 2 * sizeof(float), mesh_data->uv_list.get());
                glEnableVertexAttribArray(uv_attrib_idx);
            }

            GLuint color_attrib_idx = getAttribLocation(program, "aColor");
            if (mesh_data->color_list && color_attrib_idx != GLuint(-1))
            {
                glVertexAttribPointer(color_attrib_idx, 4, GL_BYTE, false, sizeof(std::uint8_t), mesh_data->color_list.get());
                glEnableVertexAttribArray(color_attrib_idx);
            }

            //CHECK_GL_ERROR();

            core::vec3f box_color(1.0f, 0.0f, 0.0f);
            if (draw_call_info.get_primitive_type() == kGlLines ||
                draw_call_info.get_primitive_type() == kGlLineLoop ||
                draw_call_info.get_primitive_type() == kGlLineStrip)
            {
                box_color = core::vec3f(0.8f, 0.0f, 0.8f);
            }

            setUniform4f(program, "uBoxColor", box_color.x, box_color.y, box_color.z, 1.0f);
            //CHECK_GL_ERROR();

            bindTexture2D(program, "colorTex", 0, mesh_data->tex_id);

            GLfloat local_mat[16] = {scale, 0.0f, 0.0f, 0.0f,
                                     0.0f, scale, 0.0f, 0.0f,
                                     0.0f, 0.0f, scale, 0.0f,
                                     GLfloat(mesh_data->translation.x) * scale,
                                     GLfloat(mesh_data->translation.y) * scale,
                                     GLfloat(mesh_data->translation.z) * scale,
                                     1.0f};
            setUniformMatrix4fv(program, "uModelMatrix", local_mat, 1, GL_FALSE);
            //CHECK_GL_ERROR();

            glDrawElements(GLuint(draw_call_info.get_primitive_type()),
                           draw_call_info.get_index_count(),
                           draw_call_info.get_index_type(),
                           draw_call_info.get_index_buffer());
            //CHECK_GL_ERROR();

            glDisableVertexAttribArray(pos_attrib_idx);

            if (uv_attrib_idx != GLuint(-1))
            {
                glDisableVertexAttribArray(uv_attrib_idx);
            }

            if (color_attrib_idx != GLuint(-1))
            {
                glDisableVertexAttribArray(color_attrib_idx);
            }
        }
    }
}

void OGLWidget::DrawMask(const MeshData* mesh_data, uint32_t draw_call_index, GLuint program, float scale)
{
    if (mesh_data && draw_call_index < mesh_data->draw_call_list.size())
    {
        const DrawCallInfo& draw_call_info = mesh_data->draw_call_list[draw_call_index];
        if (draw_call_info.is_drawable())
        {
            GLuint pos_attrib_idx = getAttribLocation(program, "aPosition");
            if (mesh_data->vertex_list && pos_attrib_idx != GLuint(-1))
            {
                glVertexAttribPointer(GLuint(pos_attrib_idx), 3, GL_FLOAT, false, 3 * sizeof(float), mesh_data->vertex_list.get());
                glEnableVertexAttribArray(GLuint(pos_attrib_idx));
            }

            GLuint uv_attrib_idx = getAttribLocation(program, "aTexcoord");
            if (mesh_data->uv_list && uv_attrib_idx != GLuint(-1))
            {
                glVertexAttribPointer(uv_attrib_idx, 2, GL_FLOAT, false, 2 * sizeof(float), mesh_data->uv_list.get());
                glEnableVertexAttribArray(uv_attrib_idx);
            }

            GLuint color_attrib_idx = getAttribLocation(program, "aColor");
            if (mesh_data->color_list && color_attrib_idx != GLuint(-1))
            {
                glVertexAttribPointer(color_attrib_idx, 4, GL_BYTE, false, sizeof(std::uint8_t), mesh_data->color_list.get());
                glEnableVertexAttribArray(color_attrib_idx);
            }

            //CHECK_GL_ERROR();

            core::vec3f box_color(0.0f, 1.0f, 0.0f);

            setUniform4f(program, "uBoxColor", box_color.x, box_color.y, box_color.z, 0.5f);
            //CHECK_GL_ERROR();

            bindTexture2D(program, "colorTex", 0, mesh_data->tex_id);

            GLfloat local_mat[16] = {scale, 0.0f, 0.0f, 0.0f,
                                     0.0f, scale, 0.0f, 0.0f,
                                     0.0f, 0.0f, scale, 0.0f,
                                     GLfloat(mesh_data->translation.x) * scale,
                                     GLfloat(mesh_data->translation.y) * scale,
                                     GLfloat(mesh_data->translation.z) * scale,
                                     1.0f};
            setUniformMatrix4fv(program, "uModelMatrix", local_mat, 1, GL_FALSE);
            //CHECK_GL_ERROR();

            glDrawElements(GLuint(draw_call_info.get_primitive_type()),
                           draw_call_info.get_index_count(),
                           draw_call_info.get_index_type(),
                           draw_call_info.get_index_buffer());
            //CHECK_GL_ERROR();

//            QLoggingCategory category("draw call info");
//            qCInfo(category) << draw_call_info.get_primitive_type() << draw_call_info.get_index_count() << draw_call_info.get_index_type();

            glDisableVertexAttribArray(pos_attrib_idx);

            if (uv_attrib_idx != GLuint(-1))
            {
                glDisableVertexAttribArray(uv_attrib_idx);
            }

            if (color_attrib_idx != GLuint(-1))
            {
                glDisableVertexAttribArray(color_attrib_idx);
            }
        }
    }
}
