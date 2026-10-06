#include "app.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "meshdata.h"
#include "objectclass.h"
#include "scenefile.h"
#include "segmenter.h"
#include "refiner.h"

// Headless segmentation, for testing and batch pipelines:
//   MeshTool.exe --segment in.mtscene out.mtscene [--no-model] [--res M_PER_PX]
//                [--model NAME] [--server URL]
static int RunSegmentCommand(int argc, char* argv[])
{
    if (argc < 4)
    {
        fprintf(stderr, "usage: MeshTool --segment in.mtscene out.mtscene [--no-model] [--res m] [--model name] [--server url] [--debug-dir dir] [--dump]\n");
        return 2;
    }
    SegmentSettings settings;
    for (int i = 4; i < argc; i++)
    {
        if (!strcmp(argv[i], "--no-model")) settings.useModel = false;
        else if (!strcmp(argv[i], "--res") && i + 1 < argc) settings.metresPerPixel = atof(argv[++i]);
        else if (!strcmp(argv[i], "--model") && i + 1 < argc) settings.model = argv[++i];
        else if (!strcmp(argv[i], "--server") && i + 1 < argc) settings.server = argv[++i];
        else if (!strcmp(argv[i], "--debug-dir") && i + 1 < argc) settings.debugDir = argv[++i];
        else if (!strcmp(argv[i], "--dump")) settings.dumpRaw = true;
    }

    std::vector<BatchMeshData*> batches;
    std::string error;
    if (!LoadScene(argv[2], batches, error))
    {
        fprintf(stderr, "Cannot open %s: %s\n", argv[2], error.c_str());
        return 1;
    }
    std::vector<GroupMeshData*> groups;
    for (BatchMeshData* b : batches)
        if (!b->is_spline_mesh)
            groups.insert(groups.end(), b->group_meshes.begin(), b->group_meshes.end());

    Segmenter segmenter;
    segmenter.Start(groups, settings);
    std::string last;
    while (segmenter.Running())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        std::string s = segmenter.Status();
        if (s != last) { printf("[%3.0f%%] %s\n", segmenter.Progress() * 100.0f, s.c_str()); last = s; }
    }
    std::unique_ptr<SegmentResult> result = segmenter.TakeResult();
    if (!result || !result->ok)
    {
        fprintf(stderr, "Segmentation failed: %s\n", result ? result->error.c_str() : "no result");
        return 1;
    }
    for (MeshData* m : ApplySegmentation(*result))
        delete m;
    printf("%s\n", result->summary.c_str());

    if (!SaveScene(argv[3], batches, error))
    {
        fprintf(stderr, "Cannot save %s: %s\n", argv[3], error.c_str());
        return 1;
    }
    printf("Saved %s\n", argv[3]);
    return 0;
}

// Headless Refine Buildings (same code path as Tools > Refine Buildings):
//   MeshTool.exe --refine in.mtscene out.mtscene [--no-glass]
static int RunRefineCommand(int argc, char* argv[])
{
    if (argc < 4)
    {
        fprintf(stderr, "usage: MeshTool --refine in.mtscene out.mtscene [--no-glass]\n");
        return 2;
    }
    RefineSettings settings;
    for (int i = 4; i < argc; i++)
        if (!strcmp(argv[i], "--no-glass")) settings.glass = false;
    settings.workDir = std::filesystem::path(argv[3]).parent_path().string();
    if (settings.workDir.empty()) settings.workDir = ".";
    BuildingRefiner refiner;
    refiner.Start(std::filesystem::absolute(argv[2]).string(), settings);
    std::string last;
    while (refiner.Running())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        std::string s = refiner.Status();
        if (s != last) { printf("[%3.0f%%] %s\n", refiner.Progress() * 100.0f, s.c_str()); last = s; }
    }
    std::unique_ptr<RefineResult> result = refiner.TakeResult();
    if (!result || !result->ok)
    {
        fprintf(stderr, "Refine failed: %s\n", result ? result->error.c_str() : "no result");
        return 1;
    }
    std::error_code ec;
    std::filesystem::rename(result->outputPath, argv[3], ec);
    if (ec)
    {
        fprintf(stderr, "Cannot move %s to %s: %s\n", result->outputPath.c_str(), argv[3], ec.message().c_str());
        return 1;
    }
    printf("%s\nSaved %s\n", result->summary.c_str(), argv[3]);
    return 0;
}

// Object list of a segmented scene, optionally without some classes:
//   MeshTool.exe --objects in.mtscene [out.mtscene] [--remove CLASS]...
// Prints one line per object (name, class, mesh count, bbox); --remove drops
// every object of that class (car, tree, ...) and its meshes, then saves out.
static int RunObjectsCommand(int argc, char* argv[])
{
    if (argc < 3)
    {
        fprintf(stderr, "usage: MeshTool --objects in.mtscene [out.mtscene] [--remove class]...\n");
        return 2;
    }
    const char* out_path = nullptr;
    bool remove[kObjClassCount] = {};
    for (int i = 3; i < argc; i++)
    {
        if (!strcmp(argv[i], "--remove") && i + 1 < argc)
        {
            const char* name = argv[++i];
            int c = 1;
            while (c < kObjClassCount && strcmp(GetObjectClassInfo(ObjectClass(c)).name, name)) c++;
            if (c == kObjClassCount)
            {
                fprintf(stderr, "Unknown class %s\n", name);
                return 2;
            }
            remove[c] = true;
        }
        else if (argv[i][0] != '-' && !out_path)
            out_path = argv[i];
    }

    std::vector<BatchMeshData*> batches;
    std::string error;
    if (!LoadScene(argv[2], batches, error))
    {
        fprintf(stderr, "Cannot open %s: %s\n", argv[2], error.c_str());
        return 1;
    }

    int counts[kObjClassCount] = {}, removed[kObjClassCount] = {};
    size_t removed_meshes = 0;
    int group_index = 0;
    for (BatchMeshData* b : batches)
    {
        if (b->is_spline_mesh)
            continue;
        for (GroupMeshData* g : b->group_meshes)
        {
            std::vector<size_t> mesh_count(g->objects.size(), 0);
            for (MeshData* m : g->meshes)
                if (m->object_id >= 0 && size_t(m->object_id) < mesh_count.size())
                    mesh_count[size_t(m->object_id)]++;

            printf("# group %d: %zu objects, %zu meshes\n", group_index++, g->objects.size(), g->meshes.size());
            printf("# id  name  class  meshes  size_x size_y size_z  center_x center_y center_z\n");
            for (size_t o = 0; o < g->objects.size(); o++)
            {
                const SceneObject& obj = g->objects[o];
                const core::vec3d lo = obj.bbox_ws.bb_min, hi = obj.bbox_ws.bb_max;
                printf("%zu %s %s %zu  %.1f %.1f %.1f  %.1f %.1f %.1f%s\n", o, obj.name.c_str(),
                       GetObjectClassInfo(obj.cls).name, mesh_count[o],
                       hi.x - lo.x, hi.y - lo.y, hi.z - lo.z,
                       (lo.x + hi.x) * 0.5, (lo.y + hi.y) * 0.5, (lo.z + hi.z) * 0.5,
                       remove[obj.cls] ? "  [removed]" : "");
                counts[obj.cls]++;
            }

            // Drop the removed classes' meshes and objects, renumber the rest.
            std::vector<int32_t> remap(g->objects.size(), -1);
            std::vector<SceneObject> kept;
            for (size_t o = 0; o < g->objects.size(); o++)
            {
                if (remove[g->objects[o].cls]) { removed[g->objects[o].cls]++; continue; }
                remap[o] = int32_t(kept.size());
                kept.push_back(g->objects[o]);
            }
            std::vector<MeshData*> meshes;
            for (MeshData* m : g->meshes)
            {
                const bool has_object = m->object_id >= 0 && size_t(m->object_id) < remap.size();
                if (has_object && remap[size_t(m->object_id)] < 0)
                {
                    delete m;
                    removed_meshes++;
                    continue;
                }
                if (has_object) m->object_id = remap[size_t(m->object_id)];
                meshes.push_back(m);
            }
            g->meshes.swap(meshes);
            g->objects.swap(kept);
        }
    }

    printf("# totals:");
    for (int c = 0; c < kObjClassCount; c++)
        if (counts[c]) printf(" %s %d", GetObjectClassInfo(ObjectClass(c)).name, counts[c]);
    printf("\n");
    for (int c = 0; c < kObjClassCount; c++)
        if (remove[c]) printf("# removed %d %s objects\n", removed[c], GetObjectClassInfo(ObjectClass(c)).name);
    if (removed_meshes) printf("# removed %zu meshes\n", removed_meshes);

    if (out_path)
    {
        if (!SaveScene(out_path, batches, error))
        {
            fprintf(stderr, "Cannot save %s: %s\n", out_path, error.c_str());
            return 1;
        }
        printf("# saved %s\n", out_path);
    }
    return 0;
}

int main(int argc, char* argv[])
{
    if (argc >= 2 && !strcmp(argv[1], "--segment"))
        return RunSegmentCommand(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "--refine"))
        return RunRefineCommand(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "--objects"))
        return RunObjectsCommand(argc, argv);

    try
    {
        MeshToolApp app;
        if (!app.Init())
        {
            fprintf(stderr, "Init failed. Press enter to exit.\n");
            getchar();
            return 1;
        }

        // MeshTool.exe scene.mtscene (e.g. double-clicked) opens that scene.
        if (argc >= 2 && argv[1][0] != '-')
            app.OpenOnStart(argv[1]);

        // MeshTool.exe scene.mtscene --shots prefix [--frame object | --box x0 y0 z0 x1 y1 z1]
        //   [--yaw deg] [--pitch deg] [--zoom k] [--no-glass]: check screenshots, then exit.
        MeshToolApp::AutoShots shots;
        for (int i = 2; i < argc; i++)
        {
            if (!strcmp(argv[i], "--shots") && i + 1 < argc) shots.prefix = argv[++i];
            else if (!strcmp(argv[i], "--frame") && i + 1 < argc) shots.frameObject = argv[++i];
            else if (!strcmp(argv[i], "--yaw") && i + 1 < argc) shots.yawDeg = atof(argv[++i]);
            else if (!strcmp(argv[i], "--pitch") && i + 1 < argc) shots.pitchDeg = atof(argv[++i]);
            else if (!strcmp(argv[i], "--zoom") && i + 1 < argc) shots.zoom = atof(argv[++i]);
            else if (!strcmp(argv[i], "--no-glass")) shots.glass = false;
            else if (!strcmp(argv[i], "--box") && i + 6 < argc)
            {
                for (int k = 0; k < 6; k++) shots.box[k] = atof(argv[++i]);
                shots.hasBox = true;
            }
        }
        if (!shots.prefix.empty())
            app.SetAutoShots(shots);

        app.MainLoop();
        app.Shutdown();
    }
    catch (const std::exception& e)
    {
        fprintf(stderr, "Fatal exception: %s\n", e.what());
        getchar();
        return 1;
    }
    catch (...)
    {
        fprintf(stderr, "Unknown fatal exception.\n");
        getchar();
        return 1;
    }

    return 0;
}
