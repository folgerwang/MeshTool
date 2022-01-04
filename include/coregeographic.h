#pragma once
#include "coremath.h"
#define GEOGRAPHICLIB_SHARED_LIB 0
#include "GeographicLib/UTMUPS.hpp"

namespace core
{
constexpr double emajor = 6378137.0;
constexpr double eflat = 0.00335281068118;

core::vec3d GetCartesianPosition(double lat, double lon, double altitude, bool sphere_model = true, double A = emajor, double FL = eflat);
core::GpsCoord FromCartesianToGpsSphereModel(core::vec3d pos_ws, double A = emajor);
core::matrix4d CreateLocalTransformMatrixByReferenceCoordinate(const core::vec3d& reference_pos);

class CoordinateTransformer
{
public:
    core::vec3d     m_origin_ecef;
    core::matrix3d  m_rot_mat;
    core::matrix3d  m_inv_rot_mat;
    bool            m_valid;

public:
    /**
     * @brief  Transform a latitude, longitude, altitude (LLA) geographic coordinate to an
     *         Earth-Centered Earth-Fixed (ECEF) cartesian coordinate.
     *         Ref:
     *         http://www.navipedia.net/index.php/Ellipsoidal_and_Cartesian_Coordinates_Conversion
     *
     * @param[in]   lla               Geographic coordinate in atitude, longitude, and altitude
     * @param[out]  ecef              Cartesian coordinate in ECEF frame
     * @param[in]   input_in_radians  Set this to true if the input unit of latitude and longitude
     *                                is in radians (default to false)
     *
     * @return  True if the conversion is successful, false otherwise
     */

    CoordinateTransformer()
    {
        m_valid = false;
    }

    CoordinateTransformer(double lat, double lon, double alt = 0.0, const bool input_in_radians = false)
    {
        m_valid = false;

        core::vec3d origin_lla(lon, lat, alt);
        if (!lla_to_ecef(origin_lla, m_origin_ecef, input_in_radians))
        {
            return;
        }

        // Rotation matrix
        if (!global_to_local_rotation_matrix(lat, lon, m_rot_mat, input_in_radians))
        {
            return;
        }

        m_inv_rot_mat = core::inverse(m_rot_mat);

        m_valid = true;
    }

    static bool lla_to_ecef(const core::GpsCoord& lla, core::vec3d& ecef, const bool input_in_radians = false)
    {
        double lat = lla.lat;
        double lon = lla.lon;
        const double alt = lla.alt;

        core::vec3d res(0, 0, 0);

        // Convert to radians if input angles in degrees
        if (!input_in_radians)
        {
            lat = core::DegreesToRadians(lat);
            lon = core::DegreesToRadians(lon);
        }

        bool failed = (lat > PI / 2 || lat < -PI / 2) || (lon > PI || lon < -PI);

        if (!failed)
        {
            // Intermidiate calculation
            const double sin_lat = std::sin(lat);
            const double cos_lat = std::cos(lat);
            const double sin_lon = std::sin(lon);
            const double cos_lon = std::cos(lon);
            const double r_n = WGS84_A / std::sqrt(1.0 - WGS84_E2 * std::pow(sin_lat, 2));

            // clang-format off
            ecef.x = (r_n + alt) * cos_lat * cos_lon;
            ecef.y = (r_n + alt) * cos_lat * sin_lon;
            ecef.z = (r_n * (1 - WGS84_E2) + alt) * sin_lat;
        }

        return !failed;
    }

    /**
     * @brief  Transform an Earth-Centered Earth-Fixed (ECEF) cartesian coordinate to a latitude,
     *         longitude, altitude (LLA) geographic coordinate.
     *         Ref:
     *         http://www.navipedia.net/index.php/Ellipsoidal_and_Cartesian_Coordinates_Conversion
     *
     * @param[in]   ecef               Cartesian coordinate in ECEF frame
     * @param[out]  lla                Geographic coordinate in atitude, longitude, and altitude
     * @param[in]   output_in_radians  Set this to true if the output unit of latitude and longitude
     *                                 is required to be in radians (default to false)
     *
     * @return  True if the conversion is successful, false otherwise
     */
    static bool ecef_to_lla(const core::vec3d& ecef, core::GpsCoord& lla, const bool output_in_radians = false)
    {
        core::GpsCoord res(0, 0, 0);

        const double x = ecef.x;
        const double y = ecef.y;
        const double z = ecef.z;
        const double rsq = x * x + y * y;

        double h = WGS84_E2 * z;
        double zp = 0;
        double r = 0;
        double sp = 0;
        double gsq = 0;
        double en = 0;
        double p = 0;

        // Iteratively convert geographic coordinate to cartesian coordinate
        for (size_t i = 0; i < MAX_ECEF_TO_LLA_ITERATION; ++i)
        {
            zp = z + h;
            r = std::sqrt(rsq + zp * zp);
            sp = zp / r;
            gsq = 1.0 - WGS84_E2 * sp * sp;
            en = WGS84_A / std::sqrt(gsq);
            p = en * WGS84_E2 * sp;

            // Break if the delta position update is small enough
            if (fabs(h - p) < EPSILON)
            {
                break;
            }

            h = p;
        }

        double lat = std::atan2(zp, std::sqrt(rsq));
        double lon = std::atan2(y, x);
        const double alt = r - en;

        bool failed = (lat > PI / 2 || lat < -PI / 2) || (lon > PI || lon < -PI);

        if (!failed)
        {
            // Convert to degrees if that's the required output format
            if (!output_in_radians)
            {
                lat = core::RadiansToDegrees(lat);
                lon = core::RadiansToDegrees(lon);
            }

            lla.lat = lat;
            lla.lon = lon;
            lla.alt = alt;
        }

        return !failed;
    }

    /**
     * @brief  Estimate a relative cartesian coordinate from one ECEF coordinate to another.
     *
     * @param[in]   target_ecef  ECEF coordinate of the target location for transform
     * @param[in]   origin_ecef  ECEF coordinate of the origin (reference) location
     * @param[out]  enu          Converted ENU coordinate from the target location to the origin
     *                           (reference) location
     *
     * @return  True if the conversion is successful, false otherwise
     */
    static bool ecef_to_enu(const core::vec3d& target_ecef, const core::vec3d& origin_ecef, core::vec3d& enu)
    {
        core::vec3d (0, 0, 0);
        core::vec3d origin_lla;

        if (!ecef_to_lla(origin_ecef, origin_lla, true))
        {
            return false;
        }

        // Rotation matrix
        core::matrix3d R;
        if (!global_to_local_rotation_matrix(origin_lla.lat, origin_lla.lon, R, true))
        {
            return false;
        }

        enu = R * (target_ecef - origin_ecef);

        return true;
    }

    /**
     * @brief  Estimate an ECEF coordinate from a relative cartesian coordinate.
     *
     * @param[in]   target_enu   ENU coordinate of the target location for transform
     * @param[in]   origin_ecef  ECEF coordinate of the origin (reference) location
     * @param[out]  target_ecef  Converted ECEF coordinate of the target location
     *
     * @return  True if the conversion is successful, false otherwise
     */
    static bool enu_to_ecef(const core::vec3d& target_enu, const core::vec3d& origin_ecef, core::vec3d& target_ecef)
    {
        core::vec3d origin_lla;

        if (!ecef_to_lla(origin_ecef, ref(origin_lla), true))
        {
            return false;
        }

        // Rotation matrix
        core::matrix3d R;

        if (!local_to_global_rotation_matrix(origin_lla.lat, origin_lla.lon, R, true))
        {
            return false;
        }

        target_ecef = R * target_enu + origin_ecef;

        return true;
    }

    /**
     * @brief  Estimate a relative cartesian coordinate from one LLA coordinate to another.
     *
     * @param[in]   target_lla        Geographic coordinate of the target location for transform
     * @param[in]   origin_lla        Geographic coordinate of the origin (reference) location
     * @param[out]  enu               Converted ENU coordinate from the target location to the origin
     *                                (reference) location
     * @param[in]   input_in_radians  Set this to true if the input unit of latitude and longitude
     *                                is in radians (default to false)
     *
     * @return  True if the conversion is successful, false otherwise
     */
    static bool lla_to_enu(const core::GpsCoord& target_lla, const core::GpsCoord& origin_lla,
                           core::vec3d& enu, const bool input_in_radians = false)
    {
        core::vec3d target_ecef;
        core::vec3d origin_ecef;

        if (!lla_to_ecef(target_lla, target_ecef, input_in_radians))
        {
            return false;
        }

        if (!lla_to_ecef(origin_lla, origin_ecef, input_in_radians))
        {
            return false;
        }

        // Rotation matrix
        core::matrix3d R;

        if (!global_to_local_rotation_matrix(origin_lla.lat, origin_lla.lon, R, input_in_radians))
        {
            return false;
        }

        enu = R * (target_ecef - origin_ecef);

        return true;
    }

    core::vec3d lla_to_enu(double lat, double lon, double alt = 0.0, const bool input_in_radians = false) const
    {
        return lla_to_enu(core::GpsCoord(lon, lat, alt), input_in_radians);
    }

    core::vec3d lla_to_enu(const core::GpsCoord& target_lla, const bool input_in_radians = false) const
    {
        core::vec3d res(-1e20, -1e20, -1e20);
        if (m_valid)
        {
            core::vec3d target_ecef;
            if (lla_to_ecef(target_lla, target_ecef, input_in_radians))
            {
                res = m_rot_mat * (target_ecef - m_origin_ecef);
            }
        }

        return res;
    }

    /**
     * @brief  Estimate a geographic coordinate from a relative cartesian coordinate.
     *
     * @param[in]   target_enu         ENU coordinate of the target location for transform
     * @param[in]   origin_lla         Geographic coordinate of the origin (reference) location
     * @param[out]  target_lla         Converted Geographic coordinate of the target location
     * @param[in]   input_in_radians  Set this to true if the input unit of latitude and longitude
     *                                is in radians (default to false)
     * @param[in]   output_in_radians  Set this to true if the output unit of latitude and longitude
                                       is required to be in radians (default to false)
     *
     * @return  True if the conversion is successful, false otherwise
     */
    static bool enu_to_lla(const core::vec3d& target_enu, const core::GpsCoord& origin_lla,
                           core::GpsCoord& target_lla, const bool input_in_radians = false,
                           const bool output_in_radians = false)
    {
        core::vec3d origin_ecef;
        core::vec3d target_ecef;

        if (!lla_to_ecef(origin_lla, ref(origin_ecef), input_in_radians))
        {
            return false;
        }

        // Rotation matrix
        core::matrix3d R;

        if (!local_to_global_rotation_matrix(origin_lla.lat, origin_lla.lon, R, input_in_radians))
        {
            return false;
        }

        target_ecef = R * target_enu + origin_ecef;

        if (!ecef_to_lla(target_ecef, target_lla, output_in_radians))
        {
            return false;
        }

        return true;
    }

    core::GpsCoord enu_to_lla(const core::vec3d& target_enu, const bool output_in_radians = false) const
    {
        core::GpsCoord res(-1e20, -1e20, -1e20);
        if (m_valid)
        {
            core::vec3d target_ecef = m_inv_rot_mat * target_enu + m_origin_ecef;

            core::GpsCoord gps_coord;
            if (ecef_to_lla(target_ecef, gps_coord, output_in_radians))
            {
                res = gps_coord;
            }
        }

        return res;
    }

    /**
     * @brief  Convert an ECEF velocity to an ENU velocity.
     *
     * @param[in]   ecef_vel         Velocity in ECEF coordinate
     * @param[in]   lla              Geographic position
     * @param[out]  enu_vel          Velocity in ENU coordinate
     * @param[in]   input_in_radians Set this to true if the input unit of latitude and longitude
     *                               is in radians (default to false)
     *
     * @return  True if the conversion is successful, false otherwise
     */
    static bool ecef_vel_to_enu(const core::vec3d& ecef_vel, const core::GpsCoord& lla,
                                core::vec3d& enu_vel, const bool input_in_radians = false)
    {
        // Rotation matrix
        core::matrix3d R;

        if (!global_to_local_rotation_matrix(lla.lat, lla.lon, R, input_in_radians))
        {
            return false;
        }

        enu_vel = R * ecef_vel;

        return true;
    }

    /**
     * @brief  Convert an ENU velocity to an ECEF velocity.
     *
     * @param[in]   enu_vel          Velocity in ENU coordinate
     * @param[in]   lla              Geographic position
     * @param[out]  ecef_vel         Velocity in ECEF coordinate
     * @param[in]   input_in_radians Set this to true if the input unit of latitude and longitude
     *                               is in radians (default to false)
     *
     * @return  True if the conversion is successful, false otherwise
     */
    static bool enu_vel_to_ecef(const core::vec3d& enu_vel, const core::GpsCoord& lla,
                                core::vec3d& ecef_vel, const bool input_in_radians = false)
    {
        // Rotation matrix
        core::matrix3d R;

        if (!local_to_global_rotation_matrix(lla.lat, lla.lon, R, input_in_radians))
        {
            return false;
        }

        ecef_vel = R * enu_vel;

        return true;
    }

    /**
     * @brief  Create a global frame (i.e., ECEF, LLA) to local frame (i.e., ENU) rotation matrix.
     *
     * @param[in]   latitude         Input latitude
     * @param[in]   longitude        Input longitude
     * @param[out]  rotation_matrix  Output rotation matrix
     * @param[in]   input_in_radians Set this to true if the input unit of latitude and longitude
     *                               is in radians (default to false)
     *
     * @return  True if the conversion is successful, false otherwise
     */
    static bool global_to_local_rotation_matrix(double latitude, double longitude,
                                                core::matrix3d& rotation_matrix,
                                                const bool input_in_radians = false)
    {
        if (!input_in_radians)
        {
            latitude = core::DegreesToRadians(latitude);
            longitude = core::DegreesToRadians(longitude);
        }

        // Check the validity of the input latitude
        if (latitude > PI / 2 || latitude < -PI / 2)
        {
            return false;
        }

        // Check the validity of the input longitude
        if (longitude > PI || longitude < -PI)
        {
            return false;
        }

        const double sin_lat = std::sin(latitude);
        const double cos_lat = std::cos(latitude);
        const double sin_lon = std::sin(longitude);
        const double cos_lon = std::cos(longitude);

        // clang-format off
        rotation_matrix = core::matrix3d(-sin_lon, -sin_lat * cos_lon, cos_lat * cos_lon,
                                          cos_lon, -sin_lat * sin_lon, cos_lat * sin_lon,
                                          0,        cos_lat,           sin_lat);

        return true;
    }

    /**
     * @brief  Create a local frame (i.e., ENU) to global frame (i.e., ECEF, LLA) rotation matrix.
     *
     * @param[in]   latitude         Input latitude
     * @param[in]   longitude        Input longitude
     * @param[out]  rotation_matrix  Output rotation matrix
     * @param[in]   input_in_radians Set this to true if the input unit of latitude and longitude
     *                               is in radians (default to false)
     *
     * @return  True if the conversion is successful, false otherwise
     */
    static bool local_to_global_rotation_matrix(double latitude, double longitude,
                                                core::matrix3d& rotation_matrix,
                                                const bool input_in_radians = false)
    {
        // Get the global to local rotation matrix
        if (!global_to_local_rotation_matrix(latitude, longitude, rotation_matrix,
                                             input_in_radians))
        {
            return false;
        }

        // Inverse to get the local to global rotation matrix
        rotation_matrix = core::inverse(rotation_matrix);

        return true;
    }

protected:
    // WGS-84 semi-major axis [m]
    static constexpr double WGS84_A = 6378137.0;

    // WGS-84 semi-minor axis [m]
    static constexpr double WGS84_B = 6356752.314245;

    // WGS-84 squred eccentricity [m^2]
    static constexpr double WGS84_E2 = 0.0066943799901975848;

    // Epsilon for tracking delta position update
    static constexpr double EPSILON = 1.0e-8;

    // Maximum iteration used to solve ECEF_TO_LLA
    static constexpr size_t MAX_ECEF_TO_LLA_ITERATION = 10;
};

struct UTMConvertor
{
    const double a = 6378.137;
    const double f = 1.0 / 298.257223563;
    const double N0 = 0;
    const double k0 = 0.9996;
    const double E0 = 500.0;
    const double Lambda0 = 0.0;

    const double n = f / (2.0 - f);
    const double n2 = n * n;
    const double n3 = n2 * n;
    const double A = a / (1.0 + n) * (1.0 + n2 / 4.0 + n2 * n2 / 64.0);
    const core::vec3d alpha = core::vec3d(1.0 / 2.0 * n - 2.0 / 3.0 * n2 + 5.0 / 16.0 * n3,
        13.0 / 48.0 * n2 - 3.0 / 5.0 * n3,
        61.0 / 240.0 * n3);
    const core::vec3d beta = core::vec3d(1.0 / 2.0 * n - 2.0 / 3.0 * n2 + 37.0 / 96.0 * n3,
        1.0 / 48.0 * n2 + 1.0 / 15.0 * n3,
        17.0 / 480.0 * n3);
    const core::vec3d delta = core::vec3d(2.0 * n - 2.0 / 3.0 * n2 - 2.0 * n3,
        7.0 / 3.0 * n2 - 8.0 / 5.0 * n3,
        56.0 / 15.0 * n3);

    core::vec2d FromGeographicCoord(const core::vec2d& gps_loc, int32_t zone) const
    {
        double lambda_0 = core::DegreesToRadians(zone * 6.0 - 183.0);
        const double& latitude = core::DegreesToRadians(gps_loc.x);
        const double& longitude = core::DegreesToRadians(gps_loc.y);
        const double n_t = 2.0 * sqrt(n) / (1.0 + n);
        double t = sinh(atanh(sin(latitude)) - n_t * atanh(n_t * sin(latitude)));
        double epsilon_t = atan(t / cos(longitude - lambda_0));
        double cos_epsilon_t[] = { cos(2.0 * epsilon_t), cos(4.0 * epsilon_t), cos(6.0 * epsilon_t) };
        double sin_epsilon_t[] = { sin(2.0 * epsilon_t), sin(4.0 * epsilon_t), sin(6.0 * epsilon_t) };

        double eta_t = atanh(sin(longitude - lambda_0) / sqrt(1.0 + t * t));
        double cosh_eta_t[] = { cosh(2.0 * eta_t), cosh(4.0 * eta_t), cosh(6.0 * eta_t) };
        double sinh_eta_t[] = { sinh(2.0 * eta_t), sinh(4.0 * eta_t), sinh(6.0 * eta_t) };
        double sigma = 1.0;
        double tau = 0.0;
        for (int i = 0; i < 3; i++)
        {
            sigma += 2.0 * i * alpha[i] * cos_epsilon_t[i] * cosh_eta_t[i];
            tau += 2.0 * i * alpha[i] * sin_epsilon_t[i] * sinh_eta_t[i];
        }

        core::vec2d utm_loc;
        utm_loc.x = E0 + k0 * A * (eta_t + alpha[0] * cos_epsilon_t[0] * sinh_eta_t[0]
            + alpha[1] * cos_epsilon_t[1] * sinh_eta_t[1]
            + alpha[2] * cos_epsilon_t[2] * sinh_eta_t[2]);
        utm_loc.y = N0 + k0 * A * (epsilon_t + alpha[0] * sin_epsilon_t[0] * cosh_eta_t[0]
            + alpha[1] * sin_epsilon_t[1] * cosh_eta_t[1]
            + alpha[2] * sin_epsilon_t[2] * cosh_eta_t[2]);

        return utm_loc * 1000.0;
    }

    core::vec2d ToGeographicCoord(const core::vec2d& utm_loc, int32_t zone) const
    {
        double E = utm_loc.x / 1000.0;
        double N = utm_loc.y / 1000.0;

        double epislon = (N - N0) / (k0 * A);
        double eta = (E - E0) / (k0 * A);

        core::vec3d cos_epislon(cos(2.0 * epislon), cos(4.0 * epislon), cos(6.0 * epislon));
        core::vec3d sin_epislon(sin(2.0 * epislon), sin(4.0 * epislon), sin(6.0 * epislon));
        core::vec3d cosh_eta(cosh(2.0 * eta), cosh(4.0 * eta), cosh(6.0 * eta));
        core::vec3d sinh_eta(sinh(2.0 * eta), sinh(4.0 * eta), sinh(6.0 * eta));
        core::vec3d i_t(2.0, 4.0, 6.0);

        double epislon_t = epislon - core::dot(beta * sin_epislon, cosh_eta);
        double eta_t = eta - core::dot(beta * cos_epislon, sinh_eta);
        double chi = asin(sin(epislon_t) / cosh(eta_t));

        double phi = chi + core::dot(delta, core::vec3d(sin(2.0 * chi), sin(4.0 * chi), sin(6.0 * chi)));
        double lambda_0 = zone * 6.0 - 183.0;
        double lambda = lambda_0 + core::RadiansToDegrees(atan(sinh(eta_t) / cos(epislon_t)));
        core::vec2d gps_loc;
        gps_loc.x = core::RadiansToDegrees(phi);
        gps_loc.y = lambda;

        return gps_loc;
    }
};

#define kUseGeographicLib 0
inline core::vec2d ToGeographicCoord(const core::vec2d& utm_loc, int32_t zone)
{
#if kUseGeographicLib == 0
    UTMConvertor convertor;
    core::vec2d gps_loc = convertor.ToGeographicCoord(utm_loc, zone);
#else
    core::vec2d gps_loc;
    GeographicLib::UTMUPS::Reverse(zone, true, utm_loc.x, utm_loc.y, gps_loc.x, gps_loc.y);
#endif
    return gps_loc;
}

inline core::vec2d FromGeographicCoord(const core::vec2d& gps_loc, int32_t zone)
{
#if kUseGeographicLib == 0
    UTMConvertor convertor;
    core::vec2d utm_loc = convertor.FromGeographicCoord(gps_loc, zone);
#else
    zone = 0;
    core::vec2d utm_loc;
    int32_t cal_zone;
    bool northp;
    GeographicLib::UTMUPS::Forward(gps_loc.x, gps_loc.y, cal_zone, northp, utm_loc.x, utm_loc.y);
#endif
    return utm_loc;
}

};
