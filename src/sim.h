#pragma once
// Drive simulation: a car drives the scene's captured route over the captured
// mesh.
//
// The mesh under a route is often tree canopy, not road: Google Earth's
// photogrammetry has one surface, and where trees overhang the road that
// surface is the canopy top, metres above the asphalt. The road profile is
// recovered from the captured surface sampled across the road width every
// 0.1 m along the route: tree tops are bumpy and sit metres above the road, while the road
// (bridge decks included) is flat and continuous, so the flat stretches are
// chained together from the longest one outward, a stretch that steps up by
// more than the tree height is canopy (one that steps down is water or a
// hole), the gaps are bridged at the lower road height, and the result is
// smoothed. The profile only guides the car; nothing is drawn for the road.
// Optionally the canopy triangles over the corridor are cut out of the scene
// meshes so the car is visible from above (undone when the simulation ends).
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "coremath.h"

struct BatchMeshData;
struct MeshData;

// The car's wheels sit this far above the recovered profile, clear of the
// captured surface's noise.
const double kCarLift = 0.10;

struct DriveRoadParams
{
    double stationStep = 0.1;       // metres between profile stations along the route
    double roadWidth = 7.0;         // asphalt width (two lanes)
    double shoulderWidth = 1.5;     // each side: the extra corridor the canopy cut clears
    double treeHeight = 2.5;        // a flat stretch stepping up this far from the road is canopy
    double roughness = 1.0;         // height range across the band above which a station is not road
    double maxGrade = 0.25;         // steepest climb a road stretch may have (station to station)
    double smoothSigma = 6.0;       // metres, final height smoothing
    bool   cutCanopy = true;        // remove canopy triangles over the corridor
    double clearWidth = 20.0;       // width of the corridor the canopy cut clears (the chase camera looks through it)
    double clearHeight = 1.5;       // the cut removes what sits higher than this over the road (above the car's roof)
};

struct RoadStation
{
    double      s = 0.0;            // metres from the start
    core::vec3d pos;                // centre line at the recovered road height (scene coordinates)
    double      heading = 0.0;      // radians clockwise from north
    double      surfaceZ = 0.0;     // top captured surface across the road here (NaN: none)
    bool        tree = false;       // surface rejected: canopy (or clutter on the road)
    bool        missing = false;    // no captured surface under this station
    bool        guided = false;     // height taken from the captured surface although the station was rejected
    bool        rough = false;      // the top surface here is too uneven to be road
};

struct DriveRoad
{
    bool        ok = false;
    std::string error;
    std::vector<RoadStation> stations;
    double      length = 0.0;
    int         sampled = 0, rejected = 0, missing = 0;   // stations with a surface / dropped as canopy / without any
    int         guided = 0;                               // rejected stations whose height still follows the surface

    // Centre-line pose at `s` metres: position, heading (rad, clockwise from
    // north) and pitch (rad, positive climbing).
    core::vec3d PoseAt(double s, double* heading, double* pitch) const;
};

// Recovers the road along `polyline` (lon, lat) over the scene's meshes.
// Scene coordinates are East/North/Up metres at (refLon, refLat). Meshes of
// segmented tree objects are ignored when sampling.
DriveRoad BuildDriveRoad(const std::vector<std::pair<double, double>>& polyline, double refLon, double refLat,
                         const std::vector<BatchMeshData*>& batches, const DriveRoadParams& params);

// Car geometry in car space: x right, y forward, z up, origin on the ground
// under the car's centre. The wheel mesh is drawn once per `wheelOffsets`.
struct CarMeshes
{
    std::unique_ptr<MeshData> body, glass, dark, lights, wheel;
    core::vec3d wheelOffsets[4];
    double      wheelRadius = 0.33;
};
CarMeshes BuildCarMeshes();

// Sky dome for the simulation view: a unit sphere around the eye with the
// gradient texture (RGBA, 1 x `height`) mapped by latitude - haze below the
// horizon, pale at it, deep blue at the zenith.
struct SkyDome
{
    std::unique_ptr<MeshData> mesh;
    std::vector<uint8_t>      gradient;   // RGBA rows, nadir first
    uint32_t                  gradientHeight = 0;
};
SkyDome BuildSkyDome();

// Column-major model matrix for a car part: translate `local` in car space
// (spun `spin` radians about the car's x axis first, for wheels), pose the
// car at `pos` with `heading`/`pitch`, all relative to the eye.
void CarModelMatrix(const core::vec3d& posRelEye, double heading, double pitch,
                    const core::vec3d& local, double spin, float out[16]);

// Cuts canopy triangles over the road corridor out of the scene meshes and
// puts them back later. The caller releases the GPU buffers of Touched()
// meshes after Apply/Restore (they re-upload on the next draw).
class CanopyCut
{
public:
    // Returns the number of triangles removed.
    int  Apply(const DriveRoad& road, const DriveRoadParams& params, const std::vector<BatchMeshData*>& batches);
    // Returns the meshes that were put back.
    std::vector<MeshData*> Restore();
    // Forget the backups without touching the meshes (the scene was deleted).
    void Forget() { m_backups.clear(); }
    bool Active() const { return !m_backups.empty(); }
    std::vector<MeshData*> Touched() const;

private:
    struct DrawCallBackup
    {
        size_t                drawCall;
        uint32_t              primitiveType;
        uint32_t              indexType;
        std::vector<uint32_t> indices;
    };
    struct Backup
    {
        MeshData*                  mesh;
        std::vector<DrawCallBackup> drawCalls;
    };
    std::vector<Backup> m_backups;
};
