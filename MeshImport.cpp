#include "assert.h"
#include <fstream>
#include <sstream>
#include <filesystem>
#include "opencv2/opencv.hpp"

#include <QMessageBox>
#include "coremath.h"
#include "corefile.h"
#include "coregeographic.h"
#include "glfunctionlist.h"
#include "GpaDumpAnalyzeTool.h"
#include "meshdata.h"
#include "worlddata.h"
#include "kmlfileparser.h"
#include <fbxsdk.h>
#include "debugout.h"

namespace fs = std::filesystem;

enum GeographyCoordinateModel
{
    kSphereModel,
    kEllipsoidModel,
    kEpsg3857,
    kUtmModel
};

struct ControlPoint
{
    float x, y, z;
    float dx, dy, dz;
    float s;
};

struct SmapSpline
{
    float lengthMeters;
    std::vector<ControlPoint> cps;
};

struct SmapLane
{
    uint32_t		globalId;
    uint32_t		number;
    uint32_t		centerLineIndex;
    std::vector<uint32_t> leftLineIndex;
    std::vector<uint32_t> rightLineIndex;
    std::vector<uint32_t> nextLaneIndices;
    std::vector<uint32_t> prevLaneIndices;
};

struct SmapWay
{
    uint32_t		globalId;
    uint32_t		nextJctId;
    uint32_t		prevJctId;
    std::vector<uint32_t> laneIndices;
};

struct SmapConnection
{
    uint32_t		incomingLaneIndex;
    uint32_t		outgoingLaneIndex;
    uint32_t		connectingLaneIndex;
};

struct SmapJunction
{
    uint32_t		globalId;
    uint32_t		type;
    std::vector<uint32_t> incomingWayIds;
    std::vector<uint32_t> outgoingWayIds;
    std::vector<SmapConnection> connections;
};

struct SmapTile
{
    std::vector<SmapSpline> sss;
    std::vector<SmapLane> lanes;
    std::vector<SmapWay> ways;
    std::vector<SmapJunction> junctions;
};

struct SmapWorld
{
    std::vector<SmapTile> sws;
};

void OutputTabs(int tabCount, std::ofstream& outFile)
{
    for (int i = 0; i < tabCount; i++)
        outFile << "\t";
}

void OutputOneLine(int tabCount, const std::string& line, std::ofstream& outFile)
{
    OutputTabs(tabCount, outFile);
    outFile << line << std::endl;
}

void GenerateStyle(int& tabCount, const std::string& style_name, uint32_t color, uint32_t width, uint32_t fill, std::ofstream& outFile)
{
    OutputOneLine(tabCount, "<Style id=\"" + style_name + "\">", outFile);
    tabCount++;
    OutputOneLine(tabCount, "<LineStyle>", outFile);
    tabCount++;
    std::stringstream colorString;
    colorString << std::hex << color;
    OutputOneLine(tabCount, "<color>" + colorString.str() + "</color>", outFile);
    OutputOneLine(tabCount, "<width>" + std::to_string(width) +"</width>", outFile);
    tabCount--;
    OutputOneLine(tabCount, "</LineStyle>", outFile);
    OutputOneLine(tabCount, "<PolyStyle>", outFile);
    tabCount++;
    OutputOneLine(tabCount, "<fill>" + std::to_string(fill) + "</fill>", outFile);
    tabCount--;
    OutputOneLine(tabCount, "</PolyStyle>", outFile);
    tabCount--;
    OutputOneLine(tabCount, "</Style>", outFile);
}

void GenerateStyleMap(int& tabCount, const std::string& map_name, const std::string& normal_name, const std::string& highlight_name, std::ofstream& outFile)
{
    OutputOneLine(tabCount, "<StyleMap id=\"" + map_name + "\">", outFile);
    tabCount++;
    OutputOneLine(tabCount, "<Pair>", outFile);
    tabCount++;
    OutputOneLine(tabCount, "<key>normal</key>", outFile);
    OutputOneLine(tabCount, "<styleUrl>#" + normal_name + "</styleUrl>", outFile);
    tabCount--;
    OutputOneLine(tabCount, "</Pair>", outFile);
    OutputOneLine(tabCount, "<Pair>", outFile);
    tabCount++;
    OutputOneLine(tabCount, "<key>highlight</key>", outFile);
    OutputOneLine(tabCount, "<styleUrl>#" + highlight_name + "</styleUrl>", outFile);
    tabCount--;
    OutputOneLine(tabCount, "</Pair>", outFile);
    tabCount--;
    OutputOneLine(tabCount, "</StyleMap>", outFile);
}

void GeneratePlaceMark(int& tabCount, const std::string& place_name, const std::string& stylemap_name, int num_pos, core::GpsCoord* pos, int tessellate, int altitude_mode, bool line_string, std::ofstream& outFile)
{
    OutputOneLine(tabCount, "<Placemark>", outFile);
    tabCount++;
    OutputOneLine(tabCount, "<name>" + place_name + "</name>", outFile);
    OutputOneLine(tabCount, "<styleUrl>#" + stylemap_name + "</styleUrl>", outFile);
    OutputOneLine(tabCount, line_string ? "<LineString>" : "<Polygon>", outFile);

    tabCount++;
    OutputOneLine(tabCount, "<tessellate>" + std::to_string(tessellate) + "</tessellate>", outFile);
    std::string altitude_name[] = { "clampToGround",
                                    "clampToSeaFloor",
                                    "relativeToGround",
                                    "relativeToSeaFloor",
                                    "Absolute" };
    if (altitude_mode > 0)
    {
        std::string mode_name = altitude_mode == 2 || altitude_mode == 4 ? "altitudeMode" : "gx:altitudeMode";
        OutputOneLine(tabCount, "<" + mode_name + ">" + altitude_name[altitude_mode] + "</" + mode_name + ">", outFile);
    }

    OutputOneLine(tabCount, line_string ? "<coordinates>" : "<outerBoundaryIs>", outFile);

    tabCount++;
    if (line_string)
    {
        std::stringstream posString;
        for (int i = 0; i < num_pos; i++)
        {
            posString << std::setprecision(16) << pos[i].lon << "," << std::setprecision(16) << pos[i].lat << "," << std::setprecision(16) << pos[i].alt << " ";
        }

        OutputOneLine(tabCount, posString.str(), outFile);
    }
    else
    {
        OutputOneLine(tabCount, "<LinearRing>", outFile);
        tabCount++;

        if (altitude_mode > 0)
        {
            std::string mode_name = altitude_mode == 2 || altitude_mode == 4 ? "altitudeMode" : "gx:altitudeMode";
            OutputOneLine(tabCount, "<" + mode_name + ">" + altitude_name[altitude_mode] + "</" + mode_name + ">", outFile);
        }

        OutputOneLine(tabCount, "<coordinates>", outFile);
        tabCount++;
        std::stringstream posString;
        for (int i = 0; i < num_pos; i++)
        {
            posString << std::setprecision(16) << pos[i].lon << "," << std::setprecision(16) << pos[i].lat << "," << std::setprecision(16) << pos[i].alt << " ";
        }

        OutputOneLine(tabCount, posString.str(), outFile);
        tabCount--;

        OutputOneLine(tabCount, "</coordinates>", outFile);

        tabCount--;
        OutputOneLine(tabCount, "</LinearRing>", outFile);
    }
    tabCount--;

    OutputOneLine(tabCount, line_string ? "</coordinates>" : "</outerBoundaryIs>", outFile);

    tabCount--;
    OutputOneLine(tabCount, line_string ? "</LineString>" : "</Polygon>", outFile);
    tabCount--;
    OutputOneLine(tabCount, "</Placemark>", outFile);
}

void SaveSmapToKmlFile(const std::string& file_name)
{
    core::vec2d gps_coords[2];
    gps_coords[0] = core::vec2d(37.317383, -121.900429);
    gps_coords[1] = core::vec2d(37.338745, -121.875452);

    core::vec2d utm_coords[2];
    utm_coords[0] = FromGeographicCoord(gps_coords[0], 10);
    utm_coords[1] = FromGeographicCoord(gps_coords[1], 10);

    core::output_debug_info("test", to_string(utm_coords[0].x) + " " + to_string(utm_coords[0].y) + " " + to_string(utm_coords[1].x) + " " + to_string(utm_coords[1].y));

    core::vec2d utm_coord(582186.375, 4137432.25);
    //int zone_id = 10;
    core::vec2d gps_coord = core::vec2d(37.37859773, -122.21851234);//, ToGeographicCoord(utm_coord, zone_id);
    core::CoordinateTransformer gps_to_env_cnvt(gps_coord.x, gps_coord.y, 0);

    std::ofstream outFile;
    outFile.open(file_name);

    if (outFile)
    {
        int tabCount = 0;
        OutputOneLine(tabCount, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>", outFile);
        OutputOneLine(tabCount, "<kml xmlns = \"http://www.opengis.net/kml/2.2\" xmlns:gx=\"http://www.google.com/kml/ext/2.2\" xmlns:kml=\"http://www.opengis.net/kml/2.2\" xmlns:atom=\"http://www.w3.org/2005/Atom\">", outFile);
        OutputOneLine(tabCount, "<Document>", outFile);
        tabCount++;

        OutputOneLine(tabCount, "<name>" + file_name.substr(file_name.rfind("/") + 1) + "</name>", outFile);

        GenerateStyle(tabCount, "inline", 0xffff00ff, 2, 0, outFile);
        GenerateStyle(tabCount, "inline0", 0xff00ffff, 2, 0, outFile);
        GenerateStyleMap(tabCount, "inline1", "inline", "inline4", outFile);
        GenerateStyleMap(tabCount, "inline2", "inline3", "inline0", outFile);
        GenerateStyle(tabCount, "inline3", 0xff0000ff, 2, 0, outFile);
        GenerateStyle(tabCount, "inline4", 0xff0000ff, 2, 0, outFile);

        OutputOneLine(tabCount, "<Folder>", outFile);
        tabCount++;
        std::string placeName = "smap splines";
        OutputOneLine(tabCount, "<name>" + placeName + "</name>", outFile);
        int32_t open_id = 1;
        OutputOneLine(tabCount, "<open>" + std::to_string(open_id) +"</open>", outFile);

        core::vec3d line_0[13] = {
            {1847.5615172545617, 3471.9843115491308, 0},
            {1996.0813652731294, 3412.7357854165002, 0},
            {2204.1154278823215, 3328.5125442324465, 0},
            {2306.4054842099576, 3291.0504716553128, 0},
            {2386.1120216081154, 3250.6658260402464, 0},
            {2439.781090122875, 3217.7204572490077, 0},
            {2508.0630238272965, 3168.0367156041561, 0},
            {2591.4891996373685, 3091.5184397019243, 0},
            {2656.8485603038575, 3016.3286060896621, 0},
            {2725.1304940082809, 2924.1347111657924, 0},
            {2795.38808903874, 2828.8259322686886, 0},
            {2865.052644002214, 2737.2527363449708, 0},
            {2895.3308781382821, 2698.1125800227364, 0}
        };

        core::vec3d line_1[67] = {
            {2198.5413297115228, 3016.6180440519011, 0},
            {2181.3260487182461, 3052.049494468296, 0},
            {2160.7077470635081, 3092.285209347931, 0},
            {2143.6926437561997, 3127.5164820783571, 0},
            {2124.6757635892086, 3171.7557506773592, 0},
            {2114.4667016048238, 3210.790399441184, 0},
            {2108.8617263977103, 3244.0198953119275, 0},
            {2106.4595941660905, 3272.2449490334625, 0},
            {2106.6597718520588, 3300.0696473830608, 0},
            {2111.6642140012673, 3349.5135358172374, 0},
            {2121.6730982996837, 3417.1735936745335, 0},
            {2128.6793173085757, 3465.4164159929028, 0},
            {2139.8892677228018, 3542.4848250907103, 0},
            {2150.6988627650912, 3614.7489697252772, 0},
            {2160.5075693775398, 3682.8093829545091, 0},
            {2170.3162759899883, 3753.2719284153645, 0},
            {2177.9230280567849, 3804.5174160232573, 0},
            {2185.5297801235811, 3840.349221811588, 0},
            {2194.7379536781245, 3865.7717879295665, 0},
            {2208.3500363239709, 3890.5938209896394, 0},
            {2229.7690487225827, 3918.2183416532685, 0},
            {2256.3926809563709, 3940.8384201676899, 0},
            {2270.8054743460912, 3949.6462383502967, 0},
            {2314.2440322012189, 3968.262763145352, 0},
            {2349.6754826176139, 3974.0679160384339, 0},
            {2393.31421815871, 3981.0741350473249, 0},
            {2420.5383834504032, 3986.2787548825017, 0},
            {2441.957395849015, 3994.0856846352667, 0},
            {2456.5703669247032, 4001.8926143880317, 0},
            {2478.5899123812192, 4018.9077176953388, 0},
            {2495.2046603165909, 4037.524242490394, 0},
            {2507.8158545325959, 4058.743077203037, 0},
            {2515.6227842853605, 4080.5624449735851, 0},
            {2520.427048748601, 4102.5819904301015, 0},
            {2521.6281148644111, 4121.9992259690298, 0},
            {2518.2250942029495, 4146.6210813431344, 0},
            {2509.0169206484065, 4174.4457796927336, 0},
            {2495.4048380025597, 4197.8665689510281, 0},
            {2483.1939991584914, 4219.2855813496399, 0},
            {2467.7803173389298, 4246.5097466413326, 0},
            {2453.9680570071146, 4272.1324904452795, 0},
            {2436.9529536998066, 4302.7596763984347, 0},
            {2424.5419371697703, 4327.3815317725403, 0},
            {2414.1326974994172, 4354.2053416922963, 0},
            {2407.5268338624624, 4379.4277301243064, 0},
            {2404.5241685729375, 4402.0478086387275, 0},
            {2403.5232801430952, 4427.0700193847688, 0},
            {2405.1247016308421, 4459.6989821976067, 0},
            {2415.7341189871636, 4508.7425152598498, 0},
            {2426.7438917154218, 4549.3785855114211, 0},
            {2434.1504660962496, 4576.6027508031129, 0},
            {2438.554375187553, 4604.62762683868, 0},
            {2437.75366444368, 4627.0475276671332, 0},
            {2434.7509991541547, 4657.8748913062564, 0},
            {2429.7465570049467, 4688.3018995734428, 0},
            {2424.1415817978332, 4714.1248210633585, 0},
            {2414.533052871353, 4749.9566268516874, 0},
            {2404.1238132010003, 4785.9886103259896, 0},
            {2394.5152842745201, 4817.2163293370486, 0},
            {2386.5081768357868, 4845.4413830585836, 0},
            {2381.5037346865784, 4867.8612838870367, 0},
            {2371.6950280741303, 4902.6922012455261, 0},
            {2361.8863214616813, 4941.7268500093514, 0},
            {2351.8774371632653, 4981.5622095170502, 0},
            {2348.6745941877721, 5001.579978113883, 0},
            {2349.6754826176139, 5019.3957921650644, 0},
            {2351.4770817913286, 5028.0034326617033, 0}
        };

        core::vec3d line_2[19] = {
            {2140.9948120074027, 3506.3272211122267, 0},
            {2161.5145111892443, 3503.8892370510175, 0},
            {2186.7070131550695, 3496.9816155442591, 0},
            {2208.8520350443837, 3482.1505458385718, 0},
            {2235.2635290408139, 3452.6915717656302, 0},
            {2260.049700329771, 3422.826267015821, 0},
            {2292.1498238023546, 3385.0375140670835, 0},
            {2314.2948456916688, 3358.8291854090876, 0},
            {2342.9288814322367, 3330.8944422831983, 0},
            {2366.8438360579935, 3313.2814013763909, 0},
            {2411.3473214785208, 3284.0328888575295, 0},
            {2454.4507796914486, 3254.1920331716565, 0},
            {2539.4520384816892, 3187.648157057145, 0},
            {2603.2712798773241, 3130.6178164061298, 0},
            {2651.057759000651, 3079.2140340822325, 0},
            {2705.1269226302325, 3013.3410389560536, 0},
            {2769.7982605812222, 2933.7131334059213, 0},
            {2824.2198085312921, 2866.7715358372839, 0},
            {2886.0822558369709, 2790.0763203949814, 0}
        };

        core::vec3d line_3[14] = {
            {2080.2190857279174, 3285.61408444688, 0},
            {2060.3406298453569, 3286.388569741006, 0},
            {2034.7826151392078, 3290.2609962116348, 0},
            {2013.0970269036873, 3299.0384962117264, 0},
            {1992.9604092564182, 3316.5934962119095, 0},
            {1973.3401151385663, 3338.7954079768474, 0},
            {1944.4259974912052, 3375.1962168007576, 0},
            {1925.0638651380618, 3397.9144520951127, 0},
            {1905.4435710202099, 3420.8908491541761, 0},
            {1881.0927051346023, 3441.2314729720447, 0},
            {1854.9633697410441, 3456.4191491695506, 0},
            {1819.6887669597402, 3474.0564505602024, 0},
            {1775.1677598282643, 3496.63283995443, 0},
            {1745.2130878731552, 3512.3035711161306, 0}
        };

/*        int count = 0;
        for (uint32_t i = 0; i < world.sws.size(); i++)
        {
            const SmapTile& tile = world.sws[i];
            for (uint32_t j = 0; j < tile.sss.size(); j++)
            {
                const SmapSpline& spline = tile.sss[j];

                std::vector<core::GpsCoord> pos_gps_list;
                for (uint32_t k = 0; k < spline.cps.size(); k++)
                {
                    const ControlPoint& p = spline.cps[k];
                    core::vec3d pd(double(p.x), double(p.y), double(p.z));

                    core::GpsCoord pos_gps;
                    if (model == kSphereModel || model == kEllipsoidModel)
                    {
//                        core::vec3d pos_gs = pd * local2global_mat;
//                        pos_gps = FromCartesianPosition(pos_gs, model);
                    }
                    else if (model == kEpsg3857)
                    {
//                        core::vec3d pd(p.x + centerPos.x, p.y + centerPos.y, p.z + centerPos.z);
//                        pos_gps = core::FromEpsg3857ToGps(pd);
                    }
                    else if (model == kUtmModel)
                    {
                        core::vec2d pd(double(p.x) + centerPos.x, double(p.y) + centerPos.y);
                        core::vec2d pos_gps_2d = ToGeographicCoord(pd, zone_idx);

                        pos_gps.lat = pos_gps_2d.x;
                        pos_gps.lon = pos_gps_2d.y;
                        pos_gps.alt = double(p.z + centerPos.z);
                    }

                    pos_gps_list.push_back(pos_gps);
                }

                std::string spline_name = "t" + std::to_string(count);
                GeneratePlaceMark(tabCount, spline_name, "inline2", int(pos_gps_list.size()), &pos_gps_list[0], 1, 4, false, outFile);
            }
        }*/

        core::GpsCoord ref_gps_pos;
        ref_gps_pos.lon = gps_coord.y;
        ref_gps_pos.lat = gps_coord.x;
        ref_gps_pos.alt = 0.0;

        int count = 0;
        std::vector<core::GpsCoord> pos_gps_list;
        for (int i = 0; i < 15; i++)
        {
            core::vec3d p = line_0[i];
            core::GpsCoord p_gps = gps_to_env_cnvt.enu_to_lla(p);
            pos_gps_list.push_back(p_gps);
        }
        string spline_name = "t" + to_string(count); count++;
        GeneratePlaceMark(tabCount, spline_name, "inline2", int(pos_gps_list.size()), &pos_gps_list[0], 1, 4, false, outFile);

        std::vector<core::GpsCoord> pos_gps_list_1;
        for (int i = 0; i < 22; i++)
        {
            core::vec3d p = line_1[i];
            core::GpsCoord p_gps;
            gps_to_env_cnvt.enu_to_lla(p, ref_gps_pos, p_gps);
            pos_gps_list_1.push_back(p_gps);
        }
        spline_name = "t" + to_string(count); count++;
        GeneratePlaceMark(tabCount, spline_name, "inline2", int(pos_gps_list_1.size()), &pos_gps_list_1[0], 1, 4, false, outFile);

        std::vector<core::GpsCoord> pos_gps_list_2;
        for (int i = 0; i < 19; i++)
        {
            core::vec3d p = line_2[i];
            core::GpsCoord p_gps;
            gps_to_env_cnvt.enu_to_lla(p, ref_gps_pos, p_gps);
            pos_gps_list_2.push_back(p_gps);
        }
        spline_name = "t" + to_string(count); count++;
        GeneratePlaceMark(tabCount, spline_name, "inline2", int(pos_gps_list_2.size()), &pos_gps_list_2[0], 1, 4, false, outFile);

        std::vector<core::GpsCoord> pos_gps_list_3;
        for (int i = 0; i < 26; i++)
        {
            core::vec3d p = line_3[i];
            core::GpsCoord p_gps;
            gps_to_env_cnvt.enu_to_lla(p, ref_gps_pos, p_gps);
            pos_gps_list_3.push_back(p_gps);
        }
        spline_name = "t" + to_string(count); count++;
        GeneratePlaceMark(tabCount, spline_name, "inline2", int(pos_gps_list_3.size()), &pos_gps_list_3[0], 1, 4, false, outFile);

        tabCount--;
        OutputOneLine(tabCount, "</Folder>", outFile);

        tabCount--;
        OutputOneLine(tabCount, "</Document>", outFile);
        OutputOneLine(tabCount, "</kml>", outFile);

        outFile.close();
    }
}

void ImportAndTransformFbxMeshFile(const string& file_name)
{
    core::vec2d ref_utm_coord(582186.375, 4137432.25);
    int zone_id = 10;
    core::vec2d ref_gps_coord = ToGeographicCoord(ref_utm_coord, zone_id);
    core::CoordinateTransformer gps_to_env_cnvt(ref_gps_coord.x, ref_gps_coord.y, 0);

    //core::output_debug_info("Import Fbx File, position : ", to_string(ref_gps_coord.x) + " " + to_string(ref_gps_coord.y));

    size_t pos_0 = file_name.rfind('.');
    size_t pos_1 = file_name.rfind('/');
    size_t pos_2 = file_name.rfind('\\');
    bool found_pos_1 = pos_1 != string::npos;
    bool found_pos_2 = pos_2 != string::npos;
    bool found_path = found_pos_1 || found_pos_2;

    pos_1 = (found_pos_1 && found_pos_2) ? max(pos_1, pos_2) : (found_pos_2 ? pos_2 : pos_1);
    string folder_name = file_name.substr(pos_1 + 1, pos_0 - pos_1 - 1);
    string root_path_name = found_path ? file_name.substr(0, pos_1) : "";

    if (pos_0 == string::npos)
    {
        return;
    }

    string dump_folder_name = root_path_name == "" ? folder_name : (root_path_name + "/" + folder_name);
    if (!fs::exists(dump_folder_name))
    {
        fs::create_directory(dump_folder_name);
    }

    string textures_folder_name = dump_folder_name + "/textures";
    if (!fs::exists(textures_folder_name))
    {
        fs::create_directory(textures_folder_name);
    }

    // Initialize the SDK manager. This object handles all our memory management.
    fbxsdk::FbxManager* lSdkManager = fbxsdk::FbxManager::Create();

    // Create the IO settings object.
    fbxsdk::FbxIOSettings *ios = fbxsdk::FbxIOSettings::Create(lSdkManager, IOSROOT);
    lSdkManager->SetIOSettings(ios);

    // set some IOSettings options
    ios->SetBoolProp(EXP_FBX_MATERIAL, true);
    ios->SetBoolProp(EXP_FBX_TEXTURE, true);

    // Create an importer using the SDK manager.
    fbxsdk::FbxImporter* lImporter = fbxsdk::FbxImporter::Create(lSdkManager, "");

    // Use the first argument as the filename for the importer.
    if (!lImporter->Initialize(file_name.c_str(), -1, lSdkManager->GetIOSettings())) {
        printf("Call to FbxExporter::Initialize() failed.\n");
        printf("Error returned: %s\n\n", lImporter->GetStatus().GetErrorString());
        exit(-1);
    }

    // Create a new scene so it can be populated by the imported file.
    fbxsdk::FbxScene* pScene = fbxsdk::FbxScene::Create(lSdkManager, "");

    // export the scene.
    lImporter->Import(pScene);

    // The file is imported; so get rid of the importer.
    lImporter->Destroy();

    // Create an importer using the SDK manager.
    fbxsdk::FbxExporter* lExporter = fbxsdk::FbxExporter::Create(lSdkManager, "");

    string export_file_name = file_name.substr(0, file_name.rfind(".")) + "_modi.fbx";

    // Use the first argument as the filename for the importer.
    if (!lExporter->Initialize(export_file_name.c_str(), -1, lSdkManager->GetIOSettings())) {
        printf("Call to FbxExporter::Initialize() failed.\n");
        printf("Error returned: %s\n\n", lImporter->GetStatus().GetErrorString());
        exit(-1);
    }

    core::vec2d max_error(0.0, 0.0);
    for (int32_t i_node = 0; i_node < pScene->GetNodeCount(); i_node++)
    {
        fbxsdk::FbxNode* lMeshNode = pScene->GetNode(i_node);

        // leaf node
        if (lMeshNode->GetChildCount() == 0)
        {
            fbxsdk::FbxMesh* mesh = lMeshNode->GetMesh();
            FbxVector4* vertices = mesh->GetControlPoints();
            for (int32_t i_cp = 0; i_cp < mesh->GetControlPointsCount(); i_cp++)
            {
                FbxVector4 v = FbxVector4(vertices[i_cp][0], -vertices[i_cp][2], vertices[i_cp][1], vertices[i_cp][3]);
                v += FbxVector4(ref_utm_coord.x, ref_utm_coord.y, 0.0, 0.0);
                core::vec2d gps_coord_2d = ToGeographicCoord(core::vec2d(v[0], v[1]), zone_id);
                core::vec2d utm_coord_2d = FromGeographicCoord(gps_coord_2d, zone_id);

                //core::output_debug_info("Import Fbx File, position : ", to_string(gps_coord_2d.x) + " " + to_string(gps_coord_2d.y));

                core::GpsCoord gps_coord(gps_coord_2d.y, gps_coord_2d.x, v[2]);
                core::vec3d pos_ws = gps_to_env_cnvt.lla_to_enu(gps_coord);
                core::GpsCoord pos_gps = gps_to_env_cnvt.enu_to_lla(pos_ws);
                core::vec3d pos_ws_1 = gps_to_env_cnvt.lla_to_enu(pos_gps);

                mesh->SetControlPointAt(FbxVector4(pos_ws.x, pos_ws.z, -pos_ws.y, v[3]), i_cp);
                max_error.x = max(max_error.x, abs(v[0] - utm_coord_2d.x));
                max_error.y = max(max_error.y, abs(v[1] - utm_coord_2d.y));

                //core::output_debug_info("Import Fbx File, position : ", to_string(v[0]) + " " + to_string(v[1]) + " " + to_string(v[2]) + " " + to_string(v[3]));
            }
        }
        else
        {
            //core::output_debug_info("Import Fbx File, num_node", to_string(lMeshNode->GetChildCount()));
        }
    }

    //core::output_debug_info("Import Fbx File, position(*1e10) : ", to_string(max_error.x * 1e10) + " " + to_string(max_error.y * 1e10));

    //core::GpsCoord pos_gps = gps_to_env_cnvt.enu_to_lla(core::vec3d(95082.773/100.0, 87578.219/100.0, 3930.404/100.0));
    //core::output_debug_info("Import Fbx File, position : ", to_string(pos_gps.x) + " " + to_string(pos_gps.y));

    // export the scene.
    lExporter->Export(pScene);

    // The file is imported; so get rid of the importer.
    lExporter->Destroy();

    // Destroy the SDK manager and all the other objects it was handling.
    lSdkManager->Destroy();

    string export_kml_file_name = file_name.substr(0, file_name.rfind(".")) + "_modi.kml";

    SaveSmapToKmlFile(export_kml_file_name);
}
