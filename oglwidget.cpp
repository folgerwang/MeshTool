#include "oglwidget.h"
#include "debugout.h"
#include "coregeographic.h"
#include "worlddata.h"
#include "opencv2/opencv.hpp"
#include <QWheelEvent>
#include <QLoggingCategory>
#include <QApplication>

OGLWidget::OGLWidget(QWidget *parent) : QOpenGLWidget(parent)
{
    regionSelectButton = parentWidget()->findChild<QPushButton *>("RegionSelectButton");
    m_star_tex_idx = GLuint(-1);
    m_quad_vert = nullptr;
    m_quad_index = nullptr;
}

void OGLWidget::checkGLError(const char* file, int32_t line)
{
    GLint error = GLint(glGetError());
    if (error)
    {
        string errorString;
        switch (error)
        {
        case GL_INVALID_ENUM: errorString = "GL_INVALID_ENUM"; break;
        case GL_INVALID_FRAMEBUFFER_OPERATION: errorString = "GL_INVALID_FRAMEBUFFER_OPERATION"; break;
        case GL_INVALID_VALUE: errorString = "GL_INVALID_VALUE"; break;
        case GL_INVALID_OPERATION: errorString = "GL_INVALID_OPERATION"; break;
        case GL_OUT_OF_MEMORY: errorString = "GL_OUT_OF_MEMORY"; break;
        default: errorString = "unknown error"; break;
        }
        string error_body = "error: " + string(file) + ", line " + to_string(line) + ": " + errorString;
        core::output_debug_info("GL error:", error_body);
        error = 0; // nice place to hang a breakpoint in compiler... :)
    }
}

void OGLWidget::initializeGL()
{
    initializeOpenGLFunctions();

    glDepthFunc(GL_LESS);
    glEnable(GL_DEPTH_TEST);

    initShaders();

    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);

    m_quad_vert = make_unique<core::vec2f[]>(4);
    m_quad_vert[0] = core::vec2f(0, 0);
    m_quad_vert[1] = core::vec2f(1, 0);
    m_quad_vert[2] = core::vec2f(1, 1);
    m_quad_vert[3] = core::vec2f(0, 1);

    m_quad_index = make_unique<uint16_t[]>(6);
    m_quad_index[0] = 0;
    m_quad_index[1] = 1;
    m_quad_index[2] = 2;
    m_quad_index[3] = 0;
    m_quad_index[4] = 2;
    m_quad_index[5] = 3;

    unique_ptr<core::Texture2DInfo> star_tex = make_unique<core::Texture2DInfo>();
    LoadTextureFromFile("star.png", star_tex.get());
    m_star_tex_idx = LoadTexture(star_tex.get());
}

void output_debug_matrix(const core::matrix4f& mat)
{
    string matrix_string;
    for (int i = 0; i < 16; i++)
    {
        matrix_string += to_string(mat._array[i]) + ", ";
    }

    core::output_debug_info("matrix", matrix_string);
}

void output_debug_bbox(const core::bounds3f bbox)
{
    string bbox_string;
    bbox_string += to_string(bbox.bb_min.x) + ", " + to_string(bbox.bb_min.y) + ", " + to_string(bbox.bb_min.z) + ", ";
    bbox_string += to_string(bbox.bb_max.x) + ", " + to_string(bbox.bb_max.y) + ", " + to_string(bbox.bb_max.z);

    core::output_debug_info("matrix", bbox_string);
}

void LoadTextureFromFile(const string& file_name, core::Texture2DInfo* tex_info)
{
    cv::Mat src_tex = cv::imread(file_name);
    core::Dxt1Convertor dxt_cvt;
    int w = src_tex.cols;
    int h = src_tex.rows;
    int memory_size = ((w + 3) / 4) * ((h + 3) / 4) * 8;
    if (memory_size > 0)
    {
        tex_info->m_mips[0].m_imageData = make_unique<char[]>(uint32_t(memory_size));
        uint8_t* img_data = reinterpret_cast<uint8_t*>(tex_info->m_mips[0].m_imageData.get());
        dxt_cvt.CompressImageDXT1(src_tex.data, w, h, src_tex.channels(), true, memory_size, img_data);
        tex_info->m_objectId = uint32_t(-1);
        tex_info->m_levelCount = 1;
        tex_info->m_internalFormat =
            tex_info->m_format =
            tex_info->m_type = NVIMAGE_COMPRESSED_RGBA_S3TC_DXT1;
        tex_info->m_mips[0].m_width = uint32_t(w);
        tex_info->m_mips[0].m_height = uint32_t(h);
        tex_info->m_mips[0].m_size = uint32_t(memory_size);
    }
}

GLuint OGLWidget::LoadTexture(const core::Texture2DInfo* tex_info)
{
    GLuint tex_id;
    glGenTextures(1, &tex_id);

    glBindTexture(GL_TEXTURE_2D, tex_id);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

    bool is_compressed_texture = false;
    if (tex_info->m_format == NVIMAGE_COMPRESSED_RGB_S3TC_DXT1 ||
        tex_info->m_format == NVIMAGE_COMPRESSED_RGBA_S3TC_DXT1 ||
        tex_info->m_format == NVIMAGE_COMPRESSED_RGBA_S3TC_DXT3 ||
        tex_info->m_format == NVIMAGE_COMPRESSED_RGBA_S3TC_DXT5)
        is_compressed_texture = true;

    for (uint32_t l = 0; l < tex_info->m_levelCount; l++)
    {
        if (is_compressed_texture)
        {
            glCompressedTexImage2D(GL_TEXTURE_2D, GLint(l), tex_info->m_internalFormat, GLsizei(tex_info->m_mips[l].m_width),
                                   GLsizei(tex_info->m_mips[l].m_height), 0, int(tex_info->m_mips[l].m_size), tex_info->m_mips[l].m_imageData.get());
        }
        else
        {
            glTexImage2D(GL_TEXTURE_2D, GLint(l), GLint(tex_info->m_internalFormat), GLsizei(tex_info->m_mips[l].m_width),
                         GLsizei(tex_info->m_mips[l].m_height), 0, tex_info->m_format, tex_info->m_type, tex_info->m_mips[l].m_imageData.get());
        }
    }

    return tex_id;
}

GLuint OGLWidget::LoadTexture(const QImage* tex_info) {
    GLuint tex_id;
    glGenTextures(1, &tex_id);

    glBindTexture(GL_TEXTURE_2D, tex_id);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

    //generate the texture
    glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA, tex_info->width(),
                  tex_info->height(),
                  0, GL_BGRA, GL_UNSIGNED_BYTE, tex_info->bits() );

    return tex_id;
}

void OGLWidget::UploadTextures(BatchMeshData* batch_meshes)
{
    for (uint32_t i_group = 0; i_group < batch_meshes->group_meshes.size(); i_group++)
    {
        GroupMeshData* group_meshes = batch_meshes->group_meshes[i_group];
        vector<GLuint> tex_id_list;

        if (group_meshes->loaded_textures.size() > 0)
        {
            tex_id_list.reserve(group_meshes->loaded_textures.size());
            tex_id_list.resize(group_meshes->loaded_textures.size());
            for (uint32_t i = 0; i < group_meshes->loaded_textures.size(); i++)
            {
                tex_id_list[i] = uint32_t(-1);
                const core::Texture2DInfo* tex_info = group_meshes->loaded_textures[i];
                if (tex_info)
                {
                    tex_id_list[i] = LoadTexture(tex_info);
                }
            }
        }

        for (uint32_t i_mesh = 0; i_mesh < group_meshes->meshes.size(); i_mesh++)
        {
            MeshData* mesh = group_meshes->meshes[i_mesh];
            if (mesh && mesh->idx_in_texture_list != uint32_t(-1) && mesh->idx_in_texture_list < tex_id_list.size())
            {
                mesh->tex_id = tex_id_list[mesh->idx_in_texture_list];
            }
        }
    }

    batch_meshes->is_texture_loaded = true;
}

void OGLWidget::DrawQuad(const core::vec4f& quad_params, uint32_t tex_id)
{
    GLuint program = m_programs[kScreenQuadProgram];
    glUseProgram(program);

    // Enable blending
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    GLuint pos_attrib_idx = getAttribLocation(program, "aPosition");
    if (m_quad_vert && pos_attrib_idx != GLuint(-1))
    {
        glVertexAttribPointer(GLuint(pos_attrib_idx), 2, GL_FLOAT, false, 2 * sizeof(float), m_quad_vert.get());
        glEnableVertexAttribArray(GLuint(pos_attrib_idx));
    }

    setUniform4f(program, "uScreenPosition", quad_params.x, quad_params.y, quad_params.z, quad_params.w);
    //CHECK_GL_ERROR();

    bindTexture2D(program, "colorTex", 0, tex_id);

    glDrawElements(GL_TRIANGLES, 6, kGlUShort, m_quad_index.get());

    //CHECK_GL_ERROR();

    glDisableVertexAttribArray(pos_attrib_idx);

    glDisable(GL_BLEND);

    glUseProgram(0);
}

void OGLWidget::DrawBatchMeshes(BatchMeshData* batch_meshes, const core::matrix4f& world_screen_mat, GLuint program, float scale, bool draw_tri_meshes)
{
    glUseProgram(program);

    setUniformMatrix4fv(program, "uViewProjMatrix", world_screen_mat._array, 1, GL_FALSE);

    if (!batch_meshes->is_texture_loaded)
    {
        UploadTextures(batch_meshes);
    }

    for (uint32_t i_group = 0; i_group < batch_meshes->group_meshes.size(); i_group++)
    {
        GroupMeshData* mesh_group = batch_meshes->group_meshes[i_group];
        for (uint32_t i_mesh = 0; i_mesh < mesh_group->meshes.size(); i_mesh++)
        {
           MeshData* mesh_data = mesh_group->meshes[i_mesh];
           if (mesh_data->culling(world_screen_mat, scale))
           {
               for (uint32_t i_draw = 0; i_draw < mesh_data->draw_call_list.size(); i_draw++)
               {
                   if (!mesh_data->draw_call_list[i_draw].is_drawable_patches())
                   {
                       if ((draw_tri_meshes && mesh_data->draw_call_list[i_draw].is_polygon()) ||
                           (!draw_tri_meshes && !mesh_data->draw_call_list[i_draw].is_polygon()))
                       {
                           Draw(mesh_group->meshes[i_mesh], i_draw, program, scale);
                       }
                   }
               }
           }
        }
    }

    glUseProgram(0);
}

void OGLWidget::DrawBatchMeshesSelectMask(BatchMeshData* batch_meshes, const core::matrix4f& world_screen_mat, GLuint program, float scale)
{
    glUseProgram(program);

    setUniformMatrix4fv(program, "uViewProjMatrix", world_screen_mat._array, 1, GL_FALSE);

    for (uint32_t i_group = 0; i_group < batch_meshes->group_meshes.size(); i_group++)
    {
        GroupMeshData* mesh_group = batch_meshes->group_meshes[i_group];
        for (uint32_t i_mesh = 0; i_mesh < mesh_group->meshes.size(); i_mesh++)
        {
           MeshData* mesh_data = mesh_group->meshes[i_mesh];
           if (mesh_data->culling(world_screen_mat, scale) && mesh_data->patch_list.size() > 0)
           {
               for (uint32_t i_draw = 0; i_draw < mesh_data->draw_call_list.size(); i_draw++)
               {
                   if (mesh_data->draw_call_list[i_draw].get_primitive_type() == kGlTriangles &&
                       mesh_data->draw_call_list[i_draw].is_drawable_patches())
                   {
                       DrawMask(mesh_group->meshes[i_mesh], i_draw, program, scale);
                   }
               }
           }
        }
    }

    glUseProgram(0);
}

void OGLWidget::DrawWorldMeshes(WorldData* world, const core::matrix4f& world_screen_mat, GLuint program, float scale, bool draw_tri_meshes)
{
    for (uint32_t i_batch = 0; i_batch < world->mesh_data_batches.size(); i_batch++)
    {
        bool draw_batches = (world->mesh_data_batches[i_batch]->is_spline_mesh && !draw_tri_meshes) ||
                            !world->mesh_data_batches[i_batch]->is_spline_mesh;
        if (draw_batches)
        {
            DrawBatchMeshes(world->mesh_data_batches[i_batch], world_screen_mat, program, scale, draw_tri_meshes);
        }
    }
}

void OGLWidget::DrawWorldMeshesSelectMask(WorldData* world, const core::matrix4f& world_screen_mat, GLuint program, float scale)
{
    for (uint32_t i_batch = 0; i_batch < world->mesh_data_batches.size(); i_batch++)
    {
        if (!world->mesh_data_batches[i_batch]->is_spline_mesh)
        {
            DrawBatchMeshesSelectMask(world->mesh_data_batches[i_batch], world_screen_mat, program, scale);
        }
    }
}

void OGLWidget::DrawReferencePosition(const core::matrix4f& world_screen_mat, float scale)
{
    // draw star on the reference point.
    core::vec3f ref_pos_ws = core::GetCartesianPosition(g_world.reference_pos.y, g_world.reference_pos.x, 0, false);
    core::vec4f screen_pos = world_screen_mat * core::vec4f(ref_pos_ws * scale, 1.0f);
    constexpr float star_radius = 24.0f;
    DrawQuad(core::vec4f(screen_pos.x / screen_pos.w,
                         screen_pos.y / screen_pos.w,
                         star_radius / bufferWidth,
                         star_radius / bufferHeight),
             m_star_tex_idx);
}

enum PatchType
{
    kSetPatchFalse,
    kSetPatchTrue,
    kSetPatchFlip
};

void UpdatePatchesInfo(const vector<core::vec2i>& select_list,
                       const DrawCallInfo* patch_info_list,
                       unique_ptr<core::vec2f[]>& pos_2d_list,
                       vector<bool>& patch_list,
                       PatchType path_type)
{
    for (uint32_t i_click = 0; i_click < select_list.size(); i_click++)
    {
        const core::vec2f& pt = core::vec2f(select_list[i_click].x + 0.5f, select_list[i_click].y + 0.5f);
        for (int32_t i_quad = 0; i_quad < patch_info_list->get_index_count(); i_quad += 4)
        {
            const core::vec2f& p0 = pos_2d_list[patch_info_list->get_index(i_quad)];
            const core::vec2f& p1 = pos_2d_list[patch_info_list->get_index(i_quad+1)];
            const core::vec2f& p2 = pos_2d_list[patch_info_list->get_index(i_quad+2)];
            const core::vec2f& p3 = pos_2d_list[patch_info_list->get_index(i_quad+3)];

            core::vec2f l0 = p0 - pt;
            core::vec2f l1 = p1 - pt;
            core::vec2f l2 = p2 - pt;
            core::vec2f l3 = p3 - pt;

            float v0 = cross(l0, l1);
            float v1 = cross(l1, l2);
            float v2 = cross(l2, l3);
            float v3 = cross(l3, l0);

            if ((v0 > 0 && v1 > 0 && v2 > 0 && v3 > 0) ||
                (v0 < 0 && v1 < 0 && v2 < 0 && v3 < 0))
            {
                patch_list[uint32_t(i_quad / 4)] = path_type == kSetPatchFalse ? false : (path_type == kSetPatchTrue ? true : !patch_list[uint32_t(i_quad / 4)]);
            }
        }
    }
}

void OGLWidget::SelectPatches(const core::matrix4f& world_screen_mat, const float& scale)
{
    if (QApplication::keyboardModifiers().testFlag(Qt::ControlModifier) ||
        QApplication::keyboardModifiers().testFlag(Qt::ShiftModifier) ||
        QApplication::keyboardModifiers().testFlag(Qt::AltModifier))
    {
        for (uint32_t i_batch = 0; i_batch < g_world.mesh_data_batches.size(); i_batch++)
        {
            BatchMeshData* batch_mesh_data = g_world.mesh_data_batches[i_batch];
            for (uint32_t i_group = 0; i_group < batch_mesh_data->group_meshes.size(); i_group++)
            {
                GroupMeshData* group_mesh_data = batch_mesh_data->group_meshes[i_group];
                for (uint32_t i_mesh = 0; i_mesh < group_mesh_data->meshes.size(); i_mesh++)
                {
                    MeshData* mesh_data = group_mesh_data->meshes[i_mesh];
                    DrawCallInfo* patch_info_list = nullptr;
                    DrawCallInfo* mesh_draw_list = nullptr;
                    DrawCallInfo* patches_draw_list = nullptr;
                    for (uint32_t i_draw = 0; i_draw < mesh_data->draw_call_list.size(); i_draw++)
                    {
                        DrawCallInfo& draw_call_info = mesh_data->draw_call_list[i_draw];
                        if (draw_call_info.get_primitive_type() == kGlQuads && !draw_call_info.is_drawable())
                        {
                            patch_info_list = &draw_call_info;
                        }
                        else if (draw_call_info.get_primitive_type() == kGlTriangles &&
                                 !draw_call_info.is_drawable_patches() &&
                                 draw_call_info.is_drawable())
                        {
                            mesh_draw_list = &draw_call_info;
                        }
                        else if (draw_call_info.get_primitive_type() == kGlTriangles &&
                                 draw_call_info.is_drawable_patches() &&
                                 draw_call_info.is_drawable())
                        {
                            patches_draw_list = &draw_call_info;
                        }
                    }

                    if (patch_info_list && mesh_draw_list && patches_draw_list)
                    {
                        // convert vertex to 2d position.
                        unique_ptr<core::vec2f[]> pos_2d_list = make_unique<core::vec2f[]>(uint32_t(mesh_data->num_vertex));
                        for (uint32_t i_vert = 0; i_vert < uint32_t(mesh_data->num_vertex); i_vert++)
                        {
                            core::vec4f pos_ss = world_screen_mat * core::vec4f(mesh_data->vertex_list[i_vert] * scale, 1.0f);
                            pos_2d_list[i_vert] = ((core::vec2f(pos_ss.x, pos_ss.y) / pos_ss.w) * core::vec2f(0.5f, -0.5f) + 0.5f) * core::vec2f(bufferWidth, bufferHeight);
                        }

                        UpdatePatchesInfo(select_move_list, patch_info_list, pos_2d_list, mesh_data->patch_list, kSetPatchTrue);
                        UpdatePatchesInfo(select_remove_list, patch_info_list, pos_2d_list, mesh_data->patch_list, kSetPatchFalse);
                        UpdatePatchesInfo(select_click_list, patch_info_list, pos_2d_list, mesh_data->patch_list, kSetPatchFlip);

                        patches_draw_list->clear_index_buffer();
                        for (uint32_t i_block = 0; i_block < mesh_data->patch_list.size(); i_block++)
                        {
                            if (mesh_data->patch_list[i_block])
                            {
                                patches_draw_list->add_index(mesh_draw_list->get_index(int32_t(i_block * 6)));
                                patches_draw_list->add_index(mesh_draw_list->get_index(int32_t(i_block * 6 + 1)));
                                patches_draw_list->add_index(mesh_draw_list->get_index(int32_t(i_block * 6 + 2)));
                                patches_draw_list->add_index(mesh_draw_list->get_index(int32_t(i_block * 6 + 3)));
                                patches_draw_list->add_index(mesh_draw_list->get_index(int32_t(i_block * 6 + 4)));
                                patches_draw_list->add_index(mesh_draw_list->get_index(int32_t(i_block * 6 + 5)));
                            }
                        }
                    }
                }
            }
        }

        select_click_list.clear();
        select_move_list.clear();
        select_remove_list.clear();
    }
}

void OGLWidget::paintGL()
{
    glClearDepthf(1.0f);
    glClearColor(0.35f,0.35f,0.35f,1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glPolygonMode( GL_FRONT_AND_BACK, GL_FILL );

    float fov_y = 45.0f;
    float fov_radians_y = core::DegreesToRadians(fov_y);
    float aspect_x = float(bufferWidth) / float(bufferHeight);
    float tan_fov_y = tan(fov_radians_y * 0.5f);

    bool no_real_meshes = g_world.mesh_data_batches.size() == 0 && g_world.earth_mesh;
    core::bounds3f bbox_ws = no_real_meshes ? g_world.earth_mesh->bbox_ws : g_world.bbox_ws;

    constexpr float far_clip_dist = 2000.0f;
    if (no_real_meshes)
    {
        if (bbox_ws != last_bbox_ws)
        {
            camera_ctrl.InitSphere(tan_fov_y);
            last_bbox_ws = bbox_ws;
        }
        else
        {
            camera_ctrl.UpdateOnSphere(tan_fov_y, aspect_x, bufferWidth, bufferHeight);
        }

        GLuint program = m_programs[kBasicLightProgram];

        glUseProgram(program);

        core::vec3f ref_pos_ws = core::GetCartesianPosition(g_world.reference_pos.y, g_world.reference_pos.x, 0, false);
        core::vec3f up_vector = normalize(ref_pos_ws);
        core::vec3f north_vector = core::vec3f(0, 0, 1.0f);
        core::vec3f right_vector = normalize(cross(up_vector, north_vector));
        north_vector = normalize(cross(right_vector, up_vector));

        core::vec3f eye_pos = camera_ctrl.cur_camera_info.focus_point + up_vector * camera_ctrl.cur_camera_info.dist_to_focus_point;

        float scale = far_clip_dist / length(eye_pos - camera_ctrl.cur_camera_info.focus_point);

//        QLoggingCategory category("mesh scale");
//        qCInfo(category) << scale;

        core::perspective(m_proj_mat, fov_radians_y, aspect_x, 0.05f, far_clip_dist);
        core::lookAt(m_view_mat, eye_pos * scale, camera_ctrl.cur_camera_info.focus_point * scale, north_vector);

        core::matrix4f world_screen_mat = m_proj_mat * m_view_mat;
        {
            setUniformMatrix4fv(program, "uViewProjMatrix", world_screen_mat._array, 1, GL_FALSE);

            if (g_world.earth_tex_info && g_world.earth_mesh->tex_id == uint32_t(-1))
            {
                g_world.earth_mesh->tex_id = LoadTexture(g_world.earth_tex_info.get());
            }

            Draw(g_world.earth_mesh, 0, program, scale);
        }

        glPolygonMode( GL_FRONT_AND_BACK, GL_FILL );

        DrawReferencePosition(world_screen_mat, scale);

        glUseProgram(0);
    }
    else
    {
        if (bbox_ws != last_bbox_ws)
        {
            camera_ctrl.InitPlanar(bbox_ws, tan_fov_y, aspect_x);
            last_bbox_ws = bbox_ws;
        }
        else
        {
            if (!QApplication::keyboardModifiers().testFlag(Qt::ControlModifier) &&
                !QApplication::keyboardModifiers().testFlag(Qt::ShiftModifier) &&
                !QApplication::keyboardModifiers().testFlag(Qt::AltModifier))
            {
                camera_ctrl.UpdateOnPlanar(tan_fov_y, aspect_x, bufferWidth, bufferHeight);
            }
        }

        core::vec3f eye_pos = camera_ctrl.cur_camera_info.focus_point + core::vec3f(0, 0,  camera_ctrl.cur_camera_info.dist_to_focus_point);
        float scale = min(far_clip_dist / length(eye_pos - camera_ctrl.cur_camera_info.focus_point) * 0.5f, 1.0f);

//        QLoggingCategory category("mesh scale");
//        qCInfo(category) << scale;

        core::perspective(m_proj_mat, fov_radians_y, aspect_x, 0.05f, far_clip_dist);
        core::lookAt(m_view_mat, eye_pos * scale, camera_ctrl.cur_camera_info.focus_point * scale, core::vec3f(0, 1.0f, 0));

        core::matrix4f world_screen_mat = m_proj_mat * m_view_mat;

        SelectPatches(world_screen_mat, scale);

        {
            DrawWorldMeshes(&g_world, world_screen_mat, m_programs[kBasicLightProgram], scale, true);
            DrawWorldMeshesSelectMask(&g_world, world_screen_mat, m_programs[kBasicNoLightProgram], scale);
            DrawWorldMeshes(&g_world, world_screen_mat, m_programs[kBasicNoLightProgram], scale, false);
        }
    }

    glPolygonMode( GL_FRONT_AND_BACK, GL_FILL );
}

void OGLWidget::resizeGL(int w, int h)
{
    glViewport(0,0,w,h);

    bufferWidth = w;
    bufferHeight = h;
}

void OGLWidget::mouseMoveEvent(QMouseEvent * event)
{
    if ((event->buttons() & Qt::LeftButton) && ((event->buttons() & Qt::RightButton) == 0))
    {
        camera_ctrl.UpdateCursorPos(event->x(), event->y(), CameraController::kCopyBeforeUpdate);

        if (QApplication::keyboardModifiers().testFlag(Qt::ShiftModifier))
        {
            select_move_list.push_back(core::vec2i(event->x(), event->y()));
        }
        else if (QApplication::keyboardModifiers().testFlag(Qt::AltModifier))
        {
            select_remove_list.push_back(core::vec2i(event->x(), event->y()));
        }

        update();
    }
}

void OGLWidget::mousePressEvent(QMouseEvent * event)
{
    if ((event->buttons() & Qt::LeftButton) && ((event->buttons() & Qt::RightButton) == 0))
    {
        camera_ctrl.UpdateCursorPos(event->x(), event->y(), CameraController::kCopyAfterUpdate);

        if (QApplication::keyboardModifiers().testFlag(Qt::ControlModifier))
        {
            select_click_list.push_back(core::vec2i(event->x(), event->y()));
        }

        update();
    }
}

void OGLWidget::mouseReleaseEvent(QMouseEvent * event)
{
    if ((event->buttons() & Qt::LeftButton))
    {
        camera_ctrl.UpdateCursorPos(event->x(), event->y(), CameraController::kCopyAfterUpdate);
//        QLoggingCategory category("mouse release");
//        qCInfo(category) << event->x() << event->y();
    }
}

void OGLWidget::scrollWithPixels(QWheelEvent *event, QPoint num_pixels)
{
    QLoggingCategory category("scroll pixels");
    qCInfo(category) << event->pos() << num_pixels;
}

void OGLWidget::scrollWithDegrees(QWheelEvent *event, QPoint num_degrees)
{
    camera_ctrl.UpdateWheelAngle(num_degrees.y());
    if (num_degrees.y() != 0)
    {
        camera_ctrl.UpdateCursorPos(event->x(), event->y(), CameraController::kCopyBeforeUpdate);
        update();
    }
}

void OGLWidget::wheelEvent(QWheelEvent *event)
{
    QPoint num_pixels = event->pixelDelta();
    QPoint num_degrees = event->angleDelta() / 8;

    if (!num_pixels.isNull())
    {
        scrollWithPixels(event, num_pixels);
    }
    else if (!num_degrees.isNull())
    {
        QPoint num_steps = num_degrees / 15;
        scrollWithDegrees(event, num_steps);
    }
}
