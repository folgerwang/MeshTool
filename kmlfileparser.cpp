#include "coremath.h"
#include "corefile.h"
#include "coregeographic.h"
#include "kml/dom.h"
#include "kml/engine.h"
#include "kmlfileparser.h"
#include "debugout.h"

void CreateLineFromKml(const vector<kmldom::ElementPtr>& place_marks, vector<core::vec3d>& polygon, GpsConvertMode cvt_model = kCvtSphereModel)
{
    for (uint32_t i_coord = 0; i_coord < place_marks.size(); i_coord++)
    {
        kmldom::CoordinatesPtr element = kmldom::AsCoordinates(place_marks[i_coord]);

        for (uint32_t i_point = 0; i_point < element->get_coordinates_array_size(); i_point++)
        {
            kmlbase::Vec3 coord = element->get_coordinates_array_at(i_point);
            if (cvt_model == kCvtNone)
            {
                polygon.push_back(core::GpsCoord(coord.get_heading(), coord.get_pitch(), coord.get_roll()));
            }
            else
            {
                polygon.push_back(core::GetCartesianPosition(coord.get_pitch(), coord.get_heading(), coord.get_roll(), cvt_model == kCvtSphereModel));
            }
        }
    }
}

pair<uint32_t, unique_ptr<core::vec3d[]>> FindLineFromKml(const kmldom::ElementPtr& place_mark, GpsConvertMode cvt_model = kCvtSphereModel)
{
    vector<core::vec3d> polyline;
    vector<kmldom::ElementPtr> coordinate_placemarks;
    kmlengine::GetElementsById(place_mark, kmldom::Type_coordinates, &coordinate_placemarks);

    if (coordinate_placemarks.size() > 0)
    {
        CreateLineFromKml(coordinate_placemarks, polyline, cvt_model);
    }
    else
    {
        vector<kmldom::ElementPtr> outer_placemarks;
        kmlengine::GetElementsById(place_mark, kmldom::Type_outerBoundaryIs, &outer_placemarks);

        for (uint32_t i = 0; i < outer_placemarks.size(); i++)
        {
            vector<kmldom::ElementPtr> linear_ring_placemarks;
            kmlengine::GetElementsById(outer_placemarks[i], kmldom::Type_LinearRing, &linear_ring_placemarks);

            for (uint32_t j = 0; j < linear_ring_placemarks.size(); j++)
            {
                vector<kmldom::ElementPtr> coordinate_1_placemarks;
                kmlengine::GetElementsById(linear_ring_placemarks[j], kmldom::Type_coordinates, &coordinate_1_placemarks);

                if (coordinate_1_placemarks.size() > 0)
                {
                    CreateLineFromKml(coordinate_1_placemarks, polyline, cvt_model);
                }
            }
        }
    }

    unique_ptr<core::vec3d[]> ptr = make_unique<core::vec3d[]>(polyline.size());
    memcpy(ptr.get(), polyline.data(), polyline.size() * sizeof(core::vec3d));

    return make_pair(uint32_t(polyline.size()), move(ptr));
}

void ParseGoogleEarthKmlFile(const string& file_name,
                             vector<pair<uint32_t, unique_ptr<core::vec3d[]>>>& lines,
                             vector<pair<uint32_t, unique_ptr<core::vec3d[]>>>& polygons,
                             GpsConvertMode cvt_model/* = kCvtSphereModel*/)
{
    uint32_t length = 0;
    char* kml_src = core::LoadFileToMemory(file_name, length);
    if (length > 0)
    {
        string errors;
        kmldom::ElementPtr element = kmldom::Parse(kml_src,	&errors);

        vector<kmldom::ElementPtr> doc_placemarks;
        kmlengine::GetElementsById(element, kmldom::Type_Document, &doc_placemarks);

        uint64_t num_docs = doc_placemarks.size();
        for (uint64_t i_doc = 0; i_doc < num_docs; i_doc++)
        {
            vector<kmldom::ElementPtr> style_placemarks;
            vector<kmldom::ElementPtr> style_map_placemarks;
            vector<kmldom::ElementPtr> folder_placemarks;
            kmlengine::GetElementsById(doc_placemarks[i_doc], kmldom::Type_Style, &style_placemarks);
            kmlengine::GetElementsById(doc_placemarks[i_doc], kmldom::Type_StyleMap, &style_map_placemarks);
            kmlengine::GetElementsById(doc_placemarks[i_doc], kmldom::Type_Folder, &folder_placemarks);

            if (folder_placemarks.size() > 0)
            {
                for (uint64_t i_folder = 0; i_folder < folder_placemarks.size(); i_folder++)
                {
                    vector<kmldom::ElementPtr> placemark_placemarks;
                    kmlengine::GetElementsById(folder_placemarks[i_folder], kmldom::Type_Placemark, &placemark_placemarks);

                    for (uint64_t i_place = 0; i_place < placemark_placemarks.size(); i_place++)
                    {
                        vector<kmldom::ElementPtr> linestring_placemarks;
                        vector<kmldom::ElementPtr> polygon_placemarks;
                        kmlengine::GetElementsById(placemark_placemarks[i_place], kmldom::Type_LineString, &linestring_placemarks);
                        kmlengine::GetElementsById(placemark_placemarks[i_place], kmldom::Type_Polygon, &polygon_placemarks);

                        for (uint64_t i_line = 0; i_line < linestring_placemarks.size(); i_line++)
                        {
                            lines.push_back(FindLineFromKml(linestring_placemarks[i_line], cvt_model));
                        }

                        for (uint64_t i_poly = 0; i_poly < polygon_placemarks.size(); i_poly++)
                        {
                            polygons.push_back(FindLineFromKml(polygon_placemarks[i_poly], cvt_model));
                        }
                    }
                }
            }
            else
            {
                vector<kmldom::ElementPtr> placemark_placemarks;
                kmlengine::GetElementsById(doc_placemarks[i_doc], kmldom::Type_Placemark, &placemark_placemarks);

                for (uint64_t i_place = 0; i_place < placemark_placemarks.size(); i_place++)
                {
                    vector<kmldom::ElementPtr> linestring_placemarks;
                    vector<kmldom::ElementPtr> polygon_placemarks;
                    kmlengine::GetElementsById(placemark_placemarks[i_place], kmldom::Type_LineString, &linestring_placemarks);
                    kmlengine::GetElementsById(placemark_placemarks[i_place], kmldom::Type_Polygon, &polygon_placemarks);

                    for (uint64_t i_line = 0; i_line < linestring_placemarks.size(); i_line++)
                    {
                        lines.push_back(FindLineFromKml(linestring_placemarks[i_line], cvt_model));
                    }

                    for (uint64_t i_poly = 0; i_poly < polygon_placemarks.size(); i_poly++)
                    {
                        polygons.push_back(FindLineFromKml(polygon_placemarks[i_poly], cvt_model));
                    }
                }
            }
        }

        delete[] kml_src;
    }
}

void ParseDumpTextureKmlFile(const string& file_name)
{
    uint32_t length = 0;
    char* kml_src = core::LoadFileToMemory(file_name, length);
    if (length > 0)
    {
        string errors;
        string kml_data(kml_src);
        kmldom::ElementPtr element_0 = kmldom::ParseKml(kml_data);
        kmldom::ElementPtr element_1 = kmldom::ParseKml(kml_data);

        delete[] kml_src;
    }
}
