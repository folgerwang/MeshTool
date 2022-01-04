#include "coregeographic.h"

namespace core
{
core::vec3d GetCartesianPosition(double lat, double lon, double altitude, bool sphere_model/* = true*/, double A/* = emajor*/, double FL/* = eflat*/)
{
    core::vec3d position;

    double lat_rad = core::DegreesToRadians(lat);
    double lon_rad = core::DegreesToRadians(lon);

    if (sphere_model)
    {

        double g = A + altitude;

        position.x = g * cos(lat_rad);
        position.y = position.x * sin(lon_rad);
        position.x = position.x * cos(lon_rad);
        position.z = g * sin(lat_rad);
    }
    else
    {
        double flatfn = (2.0 - FL)*FL;
        double funsq = (1.0 - FL)*(1.0 - FL);
        double g1;
        double g2;
        double sin_lat;

        sin_lat = sin(lat_rad);

        g1 = A / sqrt(1.0 - flatfn * sin_lat*sin_lat);
        g2 = g1 * funsq + altitude;
        g1 = g1 + altitude;

        position.x = g1 * cos(lat_rad);
        position.y = position.x * sin(lon_rad);
        position.x = position.x * cos(lon_rad);
        position.z = g2 * sin_lat;
    }

    return position;
}

core::GpsCoord FromCartesianToGpsSphereModel(core::vec3d pos_ws, double A/* = emajor*/)
{
    core::GpsCoord r;

    double lon_rad = atan2(pos_ws.y, pos_ws.x);
    double lat_rad = atan2(pos_ws.z, sqrt(pos_ws.y * pos_ws.y + pos_ws.x * pos_ws.x));
    double alt = pos_ws.z / sin(lat_rad) - A;

    r.lon = core::RadiansToDegrees(lon_rad);
    r.lat = core::RadiansToDegrees(lat_rad);
    r.alt = alt;

    return r;
}

core::matrix4d CreateLocalTransformMatrixByReferenceCoordinate(const core::vec3d& reference_pos)
{
    core::vec3d z_axis = normalize(reference_pos);
    core::vec3d north_pole_pos = core::emajor * core::vec3d(0, 0, 1.0);
    core::vec3d x_axis = normalize((north_pole_pos - reference_pos).cross(z_axis));
    core::vec3d y_axis = normalize(z_axis.cross(x_axis));

    core::matrix4d transform_mat;
    transform_mat.set_column(0, core::vec4d(x_axis.x, y_axis.x, z_axis.x, reference_pos.x));
    transform_mat.set_column(1, core::vec4d(x_axis.y, y_axis.y, z_axis.y, reference_pos.y));
    transform_mat.set_column(2, core::vec4d(x_axis.z, y_axis.z, z_axis.z, reference_pos.z));
    transform_mat.set_column(3, core::vec4d(0, 0, 0, 1.0));

    return inverse(transform_mat);
}

}
