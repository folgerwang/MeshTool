#include "assert.h"
#include <fstream>
#include <sstream>
#include <filesystem>
#include "opencv2/opencv.hpp"
#include "debugout.h"

#include <QMessageBox>
#include "coremath.h"
#include "corefile.h"
#include "coregeographic.h"
#include "GpaDumpAnalyzeTool.h"
#include "meshdata.h"
#include "worlddata.h"
#include "kmlfileparser.h"
#include <fbxsdk.h>

using namespace std;
namespace fs = experimental::filesystem::v1;

core::vec3d ApplyMatrix(const core::vec3f& src_point, const core::matrix4d& transform_mat)
{
    core::vec3d result;
    core::vec3d src_point_d(double(src_point.x), double(src_point.y), double(src_point.z));
    core::vec4d col_0 = transform_mat.get_column(0);
    core::vec4d col_1 = transform_mat.get_column(1);
    core::vec4d col_2 = transform_mat.get_column(2);

    result.x = src_point_d.x * col_0.x + src_point_d.y * col_0.y + src_point_d.z * col_0.z + col_0.w;
    result.y = src_point_d.x * col_1.x + src_point_d.y * col_1.y + src_point_d.z * col_1.z + col_1.w;
    result.z = src_point_d.x * col_2.x + src_point_d.y * col_2.y + src_point_d.z * col_2.z + col_2.w;

    return result;
}

cv::Point3d ApplyRotMatrix(const cv::Point3d& a, const cv::Mat& m)
{
    cv::Point3d r;
    r.x = a.x * m.at<double>(0, 0) + a.y * m.at<double>(1, 0) + a.z * m.at<double>(2, 0);
    r.y = a.x * m.at<double>(0, 1) + a.y * m.at<double>(1, 1) + a.z * m.at<double>(2, 1);
    r.z = a.x * m.at<double>(0, 2) + a.y * m.at<double>(1, 2) + a.z * m.at<double>(2, 2);

    return r;
}

core::vec3f ApplyRotMatrix(const core::vec3f& a, const cv::Mat& m)
{
    cv::Point3d r = ApplyRotMatrix(cv::Point3d(double(a.x), double(a.y), double(a.z)), m);
    return core::vec3f(float(r.x), float(r.y), float(r.z));
}

void MulMatrix(const core::matrix4d& local_mat, const cv::Mat& global_trans_mat, core::matrix4d& final_trans_mat)
{
    for (int i_row = 0; i_row < 4; i_row++)
        for (int i_col = 0; i_col < 4; i_col++)
        {
            core::vec4d row = local_mat.get_row(i_row);
            core::vec4d col(global_trans_mat.at<double>(0, i_col),
                            global_trans_mat.at<double>(1, i_col),
                            global_trans_mat.at<double>(2, i_col),
                            global_trans_mat.at<double>(3, i_col));
            final_trans_mat(i_row, i_col) = dot(row, col);
        }
}

void GenerateRotateMatrix(const cv::Point3d& axis, const double& angle, cv::Mat& rot_mat)
{
    double ca = cos(angle);
    double osca = (1.0 - ca);
    double sa = sin(angle);
    double x = axis.x;
    double y = axis.y;
    double z = axis.z;
    double xx = x * x;
    double yy = y * y;
    double zz = z * z;
    double xy = x * y;
    double xz = x * z;
    double yz = y * z;

    rot_mat.at<double>(0, 0) = ca + xx * osca;
    rot_mat.at<double>(0, 1) = xy * osca - z * sa;
    rot_mat.at<double>(0, 2) = xz * osca + y * sa;
    rot_mat.at<double>(1, 0) = xy * osca + z * sa;
    rot_mat.at<double>(1, 1) = ca + yy * osca;
    rot_mat.at<double>(1, 2) = yz * osca - x * sa;
    rot_mat.at<double>(2, 0) = xz * osca - y * sa;
    rot_mat.at<double>(2, 1) = yz * osca + x * sa;
    rot_mat.at<double>(2, 2) = ca + zz * osca;
}

double GetDet(const cv::Point3d& vec)
{
    return sqrt(vec.x * vec.x + vec.y * vec.y + vec.z * vec.z);
}

cv::Point3d Normalize(const cv::Point3d& vec)
{
    double det = GetDet(vec);
    return vec / det;
}

bool GetRigidTransform(const vector<cv::Point3d>& src_points, const vector<cv::Point3d>& dst_points, cv::Mat& transform_mat)
{
    uint64_t num_points = min(src_points.size(), dst_points.size());
    if (num_points < 3)
    {
        return false;
    }

    // Bring all the points to the centroid
    cv::Point3d src_centroid(0, 0, 0);
    cv::Point3d dst_centroid(0, 0, 0);

    for (uint32_t i = 0; i < num_points; i++)
    {
        src_centroid += src_points[i];
        dst_centroid += dst_points[i];
    }

    src_centroid /= double(num_points);
    dst_centroid /= double(num_points);

    vector<cv::Point3d> adj_src_points, adj_dst_points;
    adj_src_points.reserve(src_points.size());
    adj_dst_points.reserve(dst_points.size());
    for (uint32_t i = 0; i < num_points; i++)
    {
        adj_src_points.push_back(src_points[i] - src_centroid);
        adj_dst_points.push_back(dst_points[i] - dst_centroid);
    }

    double sum_src_dist = 0, sum_dst_dist = 0;
    for (uint32_t i = 0; i < num_points; i++)
    {
        sum_src_dist += sqrt(adj_src_points[i].dot(adj_src_points[i]));
        sum_dst_dist += sqrt(adj_dst_points[i].dot(adj_dst_points[i]));
    }

    double src_scale = sum_src_dist / num_points;
    double dst_scale = sum_dst_dist / num_points;

    cv::Point3d s_sum_normal(0, 0, 0), d_sum_normal(0, 0, 0);
    for (uint32_t i = 0; i < num_points; i++)
    {
        cv::Point3d s_vec_0 = Normalize(adj_src_points[i]);
        cv::Point3d s_vec_1 = Normalize(adj_src_points[(i + 1) % num_points]);
        cv::Point3d d_vec_0 = Normalize(adj_dst_points[i]);
        cv::Point3d d_vec_1 = Normalize(adj_dst_points[(i + 1) % num_points]);
        double s_weight = acos(abs(s_vec_0.dot(s_vec_1)));
        s_sum_normal += Normalize(s_vec_0.cross(s_vec_1)) * s_weight;
        double d_weight = acos(abs(d_vec_0.dot(d_vec_1)));
        d_sum_normal += Normalize(d_vec_0.cross(d_vec_1)) * d_weight;
    }

    cv::Point3d s_poly_normal = Normalize(s_sum_normal);
    cv::Point3d d_poly_normal = Normalize(d_sum_normal);

    cv::Point3d cross_vec = s_poly_normal.cross(d_poly_normal);
    double sin_a = GetDet(cross_vec);
    cv::Point3d rot_axis = cross_vec / sin_a;

    cv::Mat rot_mat_0(3, 3, CV_64F);
    GenerateRotateMatrix(rot_axis, -asin(sin_a), rot_mat_0);

    cv::Point3d& rot_axis_1 = d_poly_normal;

    double sum_rot_angle = 0.0;
    for (uint32_t i = 0; i < num_points; i++)
    {
        cv::Point3d n_s_vec_1 = Normalize(ApplyRotMatrix(adj_src_points[i], rot_mat_0));
        cv::Point3d n_d_vec_1 = Normalize(adj_dst_points[i]);

        cv::Point3d n_s_norm_1 = Normalize(rot_axis_1.cross(n_s_vec_1));
        cv::Point3d n_d_norm_1 = Normalize(rot_axis_1.cross(n_d_vec_1));

        cv::Point3d cross_vec_1 = n_s_norm_1.cross(n_d_norm_1);
        double sin_a_1 = GetDet(cross_vec_1);

        if (rot_axis_1.dot(cross_vec_1) < 0.0)
        {
            sin_a_1 = -sin_a_1;
        }

        sum_rot_angle += -asin(sin_a_1);
    }

    cv::Mat rot_mat_1(3, 3, CV_64F);
    GenerateRotateMatrix(rot_axis_1, sum_rot_angle / num_points, rot_mat_1);

    double scale_value = dst_scale / src_scale;
    cv::Mat scale_mat = (cv::Mat_<double>(3, 3) << scale_value, 0, 0, 0, scale_value, 0, 0, 0, scale_value);

    cv::Mat rot_scale_mat = rot_mat_0 * rot_mat_1 * scale_mat;

    cv::Mat rot_scale_mat4x4 = (cv::Mat_<double>(4, 4) <<
        rot_scale_mat.at<double>(0, 0), rot_scale_mat.at<double>(0, 1), rot_scale_mat.at<double>(0, 2), 0,
        rot_scale_mat.at<double>(1, 0), rot_scale_mat.at<double>(1, 1), rot_scale_mat.at<double>(1, 2), 0,
        rot_scale_mat.at<double>(2, 0), rot_scale_mat.at<double>(2, 1), rot_scale_mat.at<double>(2, 2), 0,
        0, 0, 0, 1);

    cv::Mat pre_trans_mat4x4 = (cv::Mat_<double>(4, 4) << 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -src_centroid.x, -src_centroid.y, -src_centroid.z, 1);
    cv::Mat post_trans_mat4x4 = (cv::Mat_<double>(4, 4) << 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, dst_centroid.x, dst_centroid.y, dst_centroid.z, 1);
    transform_mat = pre_trans_mat4x4 * rot_scale_mat4x4 * post_trans_mat4x4;

    return true;
}

cv::Mat EulerAnglesToRotationMatrix(core::vec3d &theta)
{
    // Calculate rotation about x axis
    cv::Mat R_x = (cv::Mat_<double>(3, 3) <<
        1, 0, 0,
        0, cos(theta[0]), -sin(theta[0]),
        0, sin(theta[0]), cos(theta[0])
        );

    // Calculate rotation about y axis
    cv::Mat R_y = (cv::Mat_<double>(3, 3) <<
        cos(theta[1]), 0, sin(theta[1]),
        0, 1, 0,
        -sin(theta[1]), 0, cos(theta[1])
        );

    // Calculate rotation about z axis
    cv::Mat R_z = (cv::Mat_<double>(3, 3) <<
        cos(theta[2]), -sin(theta[2]), 0,
        sin(theta[2]), cos(theta[2]), 0,
        0, 0, 1);


    // Combined rotation matrix
    cv::Mat R = R_z * R_y * R_x;

    return R;
}

core::vec3d RotMatrixToEulerAngles(const cv::Mat& rot_mat)
{
    core::vec3d rot_vector;
    double r32 = rot_mat.at<double>(2, 1);
    double r33 = rot_mat.at<double>(2, 2);
    double r31 = rot_mat.at<double>(2, 0);
    double r21 = rot_mat.at<double>(1, 0);
    double r11 = rot_mat.at<double>(0, 0);
    rot_vector.x = atan2(r32, r33);
    rot_vector.y = atan2(-r31, sqrt(r32 * r32 + r33 * r33));
    rot_vector.z = atan2(r21, r11);
    return rot_vector;
}

bool IsTwoBoundingBoxOverlapped(const core::bounds3f& bbox_0, const core::bounds3f& bbox_1, float vol_size[3])
{
    core::vec3f new_bbox[2];
    new_bbox[0] = core::Max(bbox_0.bb_min, bbox_1.bb_min);
    new_bbox[1] = core::Min(bbox_0.bb_max, bbox_1.bb_max);

    bool has_overlap = new_bbox[0].x < new_bbox[1].x && new_bbox[0].y < new_bbox[1].y && new_bbox[0].z < new_bbox[1].z;

    if (has_overlap)
    {
        core::vec3f edge_0 = bbox_0.GetDiagonal();
        core::vec3f edge_1 = bbox_1.GetDiagonal();
        core::vec3f edge_new = new_bbox[1] - new_bbox[0];

        vol_size[0] = edge_0.x * edge_0.y * edge_0.z;
        vol_size[1] = edge_1.x * edge_1.y * edge_1.z;
        vol_size[2] = edge_new.x * edge_new.y * edge_new.z;

        return true;
    }
    return false;
}

bool IsMeshAreTheSame(const MeshData* mesh_0, const MeshData* mesh_1)
{
    float vol_size[3];
    if (IsTwoBoundingBoxOverlapped(mesh_0->bbox_ws, mesh_1->bbox_ws, vol_size))
    {
        const DrawCallInfo* draw_call_0 = nullptr;
        for (uint32_t i_draw = 0; i_draw < mesh_0->draw_call_list.size(); i_draw++)
        {
            if (mesh_0->draw_call_list[i_draw].get_primitive_type() == kGlTriangles)
            {
                draw_call_0 = &mesh_0->draw_call_list[i_draw];
            }
        }

        const DrawCallInfo* draw_call_1 = nullptr;
        for (uint32_t i_draw = 0; i_draw < mesh_1->draw_call_list.size(); i_draw++)
        {
            if (mesh_1->draw_call_list[i_draw].get_primitive_type() == kGlTriangles)
            {
                draw_call_1 = &mesh_1->draw_call_list[i_draw];
            }
        }

        if (vol_size[2] / vol_size[0] > 0.8f && vol_size[2] / vol_size[1] > 0.8f)
        {
            if (mesh_0->num_vertex == mesh_1->num_vertex &&
                draw_call_0->get_index_count() == draw_call_1->get_index_count())
            {
                return true;
            }
        }
    }

    return false;
}

bool DumpGeFilesWithReference(const string& kml_name,
                              const string& dump_name,
                              const core::CoordinateTransformer& gps_to_env_cnvt,
                              GroupMeshData* group_mesh_data)
{
    vector<pair<uint32_t, unique_ptr<core::vec3d[]>>> lines;
    vector<pair<uint32_t, unique_ptr<core::vec3d[]>>> polys;

    ParseGoogleEarthKmlFile(kml_name, lines, polys);
    CreateMeshFromDumpFile(dump_name, group_mesh_data);

    if (polys.size() == 0 || group_mesh_data->meshes.size() == 0)
    {
        return false;
    }

    vector<cv::Point3d> target_point_list;
    core::vec3d* poly_array = polys[0].second.get();
    for (uint32_t i = 0; i < polys[0].first - 1; i++)
    {
        core::vec4d pos_ls = core::vec4d(poly_array[i].x, poly_array[i].y, poly_array[i].z, 1.0);
        target_point_list.push_back(cv::Point3d(pos_ls.x, pos_ls.y, pos_ls.z));
    }

    vector<cv::Point3d> source_point_list;
    uint32_t num_non_tri_meshes = 0;
    for (uint32_t i = 0; i < group_mesh_data->meshes.size(); i++)
    {
        MeshData* data_mesh = group_mesh_data->meshes[i];
        if (data_mesh)
        {
            bool has_lines = false;
            for (uint32_t i_draw = 0; i_draw < data_mesh->draw_call_list.size(); i_draw++)
            {
                if (!data_mesh->draw_call_list[i_draw].is_polygon())
                {
                    has_lines = true;
                }
            }

            if (has_lines)
            {
                auto& vertex_list = data_mesh->vertex_list;
                if (vertex_list)
                {
                    for (int j = 0; j < data_mesh->num_vertex - 3; j++)
                    {
                        core::vec3d transformed_position = ApplyMatrix(vertex_list[uint32_t(j + 1)], data_mesh->dumpped_matrix);
                        source_point_list.push_back(cv::Point3d(transformed_position.x, transformed_position.y, transformed_position.z));
                    }

                    num_non_tri_meshes ++;
                }
            }
        }
    }

    if (num_non_tri_meshes == 0)
    {
        return false;
    }

    cv::Mat rigid_transform_matrix;
    GetRigidTransform(source_point_list, target_point_list, rigid_transform_matrix);

    for (uint32_t iMesh = 0; iMesh < group_mesh_data->meshes.size(); iMesh++)
    {
        MeshData* mesh_data = group_mesh_data->meshes[iMesh];

        if (mesh_data)
        {
            core::matrix4d local_world_matrix;
            MulMatrix(mesh_data->dumpped_matrix, rigid_transform_matrix, local_world_matrix);

            auto& vertex_list = mesh_data->vertex_list;
            mesh_data->gps_vert_list = make_unique<core::GpsCoord[]>(uint32_t(mesh_data->num_vertex));
            for (uint32_t i = 0; i < uint32_t(mesh_data->num_vertex); i++)
            {
                core::vec4d pos_gs = core::vec4d(vertex_list[i], 1.0) * local_world_matrix;
                core::GpsCoord gps_coord = FromCartesianToGpsSphereModel(core::vec3d(pos_gs));
                mesh_data->gps_vert_list[i] = gps_coord;
                core::vec3d pos_ws = gps_to_env_cnvt.lla_to_enu(gps_coord);
                vertex_list[i] = pos_ws;
            }

            mesh_data->bbox_ws.Reset();
            mesh_data->bbox_gps.Reset();
            for (uint32_t i = 0; i < uint32_t(mesh_data->num_vertex); i++)
            {
                mesh_data->bbox_ws += vertex_list[i];
                mesh_data->bbox_gps += mesh_data->gps_vert_list[i];
            }

            mesh_data->translation = mesh_data->bbox_ws.GetCentroid();

            for (uint32_t i = 0; i < uint32_t(mesh_data->num_vertex); i++)
            {
                vertex_list[i] -= mesh_data->translation;
            }

            group_mesh_data->bbox_ws += mesh_data->bbox_ws;
            group_mesh_data->bbox_gps += mesh_data->bbox_gps;
        }
    }

    return true;
}

void DumpGoogleEarthMeshes(const vector<string>& file_name_list, BatchMeshData* batch_mesh_data, QProgressBar* progress_bar)
{
    core::CoordinateTransformer gps_to_env_cnvt(batch_mesh_data->reference_pos.y, batch_mesh_data->reference_pos.x, 0.0);

    for (uint32_t i_dump = 0; i_dump < file_name_list.size(); i_dump++)
    {
        uint64_t file_name_pos = file_name_list[i_dump].rfind('.');
        string kml_file_name = file_name_list[i_dump].substr(0, file_name_pos) + ".kml";
        GroupMeshData* group_mesh_data = new GroupMeshData;

        bool has_valid_tri_meshes = DumpGeFilesWithReference(kml_file_name, file_name_list[i_dump], gps_to_env_cnvt, group_mesh_data);

        // remove all non-tri meshes.
        if (has_valid_tri_meshes)
        {
            uint32_t i_mesh = 0;
            while (i_mesh < group_mesh_data->meshes.size())
            {
                MeshData* mesh_data = group_mesh_data->meshes[i_mesh];

                if (mesh_data)
                {
                    bool is_polygon = false;
                    for (uint32_t i_draw = 0; i_draw < mesh_data->draw_call_list.size(); i_draw++)
                    {
                        if (mesh_data->draw_call_list[i_draw].is_polygon())
                            is_polygon = true;
                    }

                    if (is_polygon)
                    {
                        i_mesh++;
                    }
                    else
                    {
                        group_mesh_data->remove_item(i_mesh);
                    }
                }
                else
                {
                    i_mesh++;
                }
            }
        }

        if (group_mesh_data->meshes.size() > 0)
        {
            for (uint32_t i = 0; i < batch_mesh_data->group_meshes.size(); i++)
            {
                for (uint32_t j = 0; j < batch_mesh_data->group_meshes[i]->meshes.size(); j++)
                {
                    uint32_t k = 0;
                    while (k < group_mesh_data->meshes.size())
                    {
                        if (IsMeshAreTheSame(group_mesh_data->meshes[k], batch_mesh_data->group_meshes[i]->meshes[j]))
                        {
                            group_mesh_data->remove_item(k);
                        }
                        else
                        {
                            k++;
                        }
                    }
                }
            }
        }

        int num_textures = 0;
        if (group_mesh_data->meshes.size() > 0)
        {
            vector<uint8_t> valid_tex_list;
            valid_tex_list.reserve(group_mesh_data->loaded_textures.size());
            valid_tex_list.resize(group_mesh_data->loaded_textures.size(), 0);

            for (uint32_t i = 0; i < group_mesh_data->meshes.size(); i++)
            {
                if (group_mesh_data->meshes[i]->idx_in_texture_list < valid_tex_list.size())
                {
                    valid_tex_list[group_mesh_data->meshes[i]->idx_in_texture_list] = 1;
                }
            }

            for (uint32_t i = 0; i < valid_tex_list.size(); i++)
            {
                if (valid_tex_list[i] == 0)
                {
                    SAFE_DELETE(group_mesh_data->loaded_textures[i]);
                }
                else
                {
                    num_textures ++;
                }
            }

            batch_mesh_data->group_meshes.push_back(group_mesh_data);
            batch_mesh_data->bbox_ws += group_mesh_data->bbox_ws;
            batch_mesh_data->bbox_gps += group_mesh_data->bbox_gps;
        }

        progress_bar->setValue(int32_t(float(i_dump + 1) / float(file_name_list.size()) * 100.0f));
    }
}

struct GpsRay
{
    core::vec3d org;
    core::vec3d dir;

    GpsRay(const core::vec3d& ray_org)
    {
        org = ray_org;
        dir = core::vec3d(0.0, 0.0, 1.0);
    }
};

bool RayBboxIntersectCheck(const GpsRay& ray, const core::bounds3d& bounds)
{
    return (ray.org.x >= bounds.bb_min.x && ray.org.x <= bounds.bb_max.x) &&
           (ray.org.y >= bounds.bb_min.y && ray.org.y <= bounds.bb_max.y);
}

void DebugDrawGpsCoord(const core::vec3d& gps_point)
{
    core::output_debug_info("gps coord", to_string(gps_point.x) + ", " + to_string(gps_point.y) + ", " + to_string(gps_point.z));
}

bool RayTriangleIntersectCheck(const GpsRay& ray, const unique_ptr<core::vec3d[]>& vertex_list, std::uint32_t* index, double& hit_t)
{
    const double EPSILON = 1e-30;
    const core::vec3d& vertex0 = vertex_list[index[0]];
    const core::vec3d& vertex1 = vertex_list[index[1]];
    const core::vec3d& vertex2 = vertex_list[index[2]];
    core::bounds3d bbox_gps;
    bbox_gps += vertex0;
    bbox_gps += vertex1;
    bbox_gps += vertex2;

    if (RayBboxIntersectCheck(ray, bbox_gps))
    {
        core::vec3d edge1 = vertex1 - vertex0;
        core::vec3d edge2 = vertex2 - vertex0;
        core::vec3d h = core::vec3d(-edge2.y, edge2.x, 0);

        double a = dot(edge1, h);
        if (abs(a) < EPSILON)
            return false;

        double f = 1 / a;
        core::vec3d s = ray.org - vertex0;
        double u = f * (dot(s, h));
        if (u < 0.0 || u > 1.0)
            return false;

        core::vec3d q = cross(s, edge1);
        double v = f * q.z;
        if (v < 0.0 || u + v > 1.0)
            return false;

        // At this stage we can compute t to find out where the intersection point is on the line.
        hit_t = f * dot(edge2, q);
        return true;
    }
    return false;
}

void RayTriangleMeshIntersectCheck(const GpsRay& ray, const MeshData* mesh_data, std::vector<double>& hit_t_list)
{
    if (RayBboxIntersectCheck(ray, mesh_data->bbox_gps))
    {
        for (uint32_t i_draw = 0; i_draw < mesh_data->draw_call_list.size(); i_draw++)
        {
            const DrawCallInfo& draw_call_info = mesh_data->draw_call_list[i_draw];
            if (draw_call_info.get_primitive_type() == kGlTriangles)
            {
                for (int i = 0; i < draw_call_info.get_index_count(); i += 3)
                {
                    double t;
                    uint32_t tri_index[3] = {draw_call_info.get_index(i), draw_call_info.get_index(i+1), draw_call_info.get_index(i+2)};
                    bool b_found = RayTriangleIntersectCheck(ray, mesh_data->gps_vert_list, tri_index, t);
                    if (b_found)
                    {
                        hit_t_list.push_back(t);
                    }
                }
            }
        }
    }
}

double FindTouchPointAltitude(const core::vec3d& cast_loc, const vector<MeshData*>& mesh_list)
{
    GpsRay ray(cast_loc);

    std::vector<double> t_list;
    for (uint32_t i_mesh = 0; i_mesh < mesh_list.size(); i_mesh++)
    {
        RayTriangleMeshIntersectCheck(ray, mesh_list[i_mesh], t_list);
    }

    if (t_list.size() > 0)
    {
        double max_t = -1e30;
        for (uint32_t i = 0; i < t_list.size(); i++)
        {
            max_t = std::max(max_t, t_list[i]);
        }
        return max_t;
    }

    return 0;
}

void FoundIntersectMeshesList(core::bounds3d ipt_bbox, vector<MeshData*>& mesh_list)
{
    core::bounds2d ipt_bbox_2d(ipt_bbox);

    for (uint32_t i_batch = 0; i_batch < g_world.mesh_data_batches.size(); i_batch++)
    {
        BatchMeshData* batch_meshes = g_world.mesh_data_batches[i_batch];

        core::bounds2d bb_intersect = core::bounds2d(batch_meshes->bbox_gps) ^ ipt_bbox_2d;
        if (batch_meshes->is_google_dump && bb_intersect.b_valid)
        {
            for (uint32_t i_group = 0; i_group < batch_meshes->group_meshes.size(); i_group++)
            {
                GroupMeshData* group_meshes = batch_meshes->group_meshes[i_group];
                bb_intersect = core::bounds2d(group_meshes->bbox_gps) ^ ipt_bbox_2d;
                if (bb_intersect.b_valid)
                {
                    for (uint32_t i_mesh = 0; i_mesh < group_meshes->meshes.size(); i_mesh++)
                    {
                        bb_intersect = core::bounds2d(group_meshes->meshes[i_mesh]->bbox_gps) ^ ipt_bbox_2d;
                        if (bb_intersect.b_valid)
                        {
                            mesh_list.push_back(group_meshes->meshes[i_mesh]);
                        }
                    }
                }
            }
        }
    }
}

void DumpKmlSplineMeshes(const vector<string>& file_name_list, BatchMeshData* batch_mesh_data, QProgressBar* progress_bar)
{
    core::CoordinateTransformer gps_to_env_cnvt(batch_mesh_data->reference_pos.y, batch_mesh_data->reference_pos.x, 0.0);

    vector<pair<uint32_t, unique_ptr<core::vec3d[]>>> lines;
    vector<pair<uint32_t, unique_ptr<core::vec3d[]>>> polys;

    for (uint32_t i = 0; i < file_name_list.size(); i++)
    {
        ParseGoogleEarthKmlFile(file_name_list[i], lines, polys, kCvtNone);

        GroupMeshData* group_mesh_data = new GroupMeshData;

        for (uint32_t i_line = 0; i_line < lines.size(); i_line++)
        {
            MeshData* mesh_data = new MeshData;

            mesh_data->num_vertex = int32_t(lines[i_line].first);
            if (mesh_data->num_vertex)
            {
                mesh_data->vertex_list = make_unique<core::vec3f[]>(uint32_t(mesh_data->num_vertex));
                mesh_data->gps_vert_list = make_unique<core::GpsCoord[]>(uint32_t(mesh_data->num_vertex));
                mesh_data->add_draw_call_list(kGlLineStrip, mesh_data->num_vertex, mesh_data->num_vertex);
                DrawCallInfo& last_draw_call_info = mesh_data->get_last_draw_call_info();
                core::vec3d* points = lines[i_line].second.get();

                for (uint32_t i_point = 0; i_point < uint32_t(mesh_data->num_vertex); i_point++)
                {
                    mesh_data->gps_vert_list[i_point] = points[i_point];
                    mesh_data->bbox_gps += mesh_data->gps_vert_list[i_point];
                }

                for (uint32_t i_line = 0; i_line < uint32_t(mesh_data->num_vertex); i_line++)
                {
                    last_draw_call_info.add_index(i_line);
                }

                group_mesh_data->meshes.push_back(mesh_data);
                group_mesh_data->bbox_gps += mesh_data->bbox_gps;
            }
        }

        for (uint32_t i_poly = 0; i_poly < polys.size(); i_poly++)
        {
            MeshData* mesh_data = new MeshData;

            mesh_data->num_vertex = int32_t(polys[i_poly].first) - 1;
            mesh_data->vertex_list = make_unique<core::vec3f[]>(uint32_t(mesh_data->num_vertex));
            mesh_data->gps_vert_list = make_unique<core::GpsCoord[]>(uint32_t(mesh_data->num_vertex));
            mesh_data->add_draw_call_list(kGlLineLoop, mesh_data->num_vertex, mesh_data->num_vertex);
            DrawCallInfo& last_draw_call_info = mesh_data->get_last_draw_call_info();
            core::vec3d* points = polys[i_poly].second.get();

            for (uint32_t i_point = 0; i_point < uint32_t(mesh_data->num_vertex); i_point++)
            {
                mesh_data->gps_vert_list[i_point] = points[i_point];
                mesh_data->bbox_gps += mesh_data->gps_vert_list[i_point];
            }

            for (uint32_t i_line = 0; i_line < uint32_t(mesh_data->num_vertex); i_line++)
            {
                last_draw_call_info.add_index(i_line);
            }

            group_mesh_data->meshes.push_back(mesh_data);
            group_mesh_data->bbox_gps += mesh_data->bbox_gps;
        }

        for (uint32_t i_mesh = 0; i_mesh < group_mesh_data->meshes.size(); i_mesh++)
        {
            MeshData* mesh_data = group_mesh_data->meshes[i_mesh];
            vector<MeshData*> mesh_list;
            FoundIntersectMeshesList(mesh_data->bbox_gps, mesh_list);

            for (uint32_t i_vert = 0; i_vert < uint32_t(mesh_data->num_vertex); i_vert++)
            {
                const core::vec3d src_gps_coord = mesh_data->gps_vert_list[i_vert];
                mesh_data->gps_vert_list[i_vert].alt = src_gps_coord.alt + FindTouchPointAltitude(src_gps_coord, mesh_list);
            }
            group_mesh_data->bbox_ws += mesh_data->bbox_ws;
        }

        for (uint32_t i_mesh = 0; i_mesh < group_mesh_data->meshes.size(); i_mesh++)
        {
            MeshData* mesh_data = group_mesh_data->meshes[i_mesh];
            for (uint32_t i_vert = 0; i_vert < uint32_t(mesh_data->num_vertex); i_vert++)
            {
                mesh_data->vertex_list[i_vert] = gps_to_env_cnvt.lla_to_enu(mesh_data->gps_vert_list[i_vert]);
                mesh_data->bbox_ws += mesh_data->vertex_list[i_vert];
            }
            group_mesh_data->bbox_ws += mesh_data->bbox_ws;
        }

        batch_mesh_data->group_meshes.push_back(group_mesh_data);
        batch_mesh_data->bbox_ws += group_mesh_data->bbox_ws;
        batch_mesh_data->bbox_gps += group_mesh_data->bbox_gps;
    }
}
