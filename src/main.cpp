#include "app.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <thread>
#include <vector>

#include "meshdata.h"
#include "scenefile.h"
#include "segmenter.h"

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

int main(int argc, char* argv[])
{
    if (argc >= 2 && !strcmp(argv[1], "--segment"))
        return RunSegmentCommand(argc, argv);

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

        // MeshTool.exe scene.mtscene --shots prefix [--frame object] [--yaw deg]
        //   [--pitch deg] [--zoom k]: check screenshots, then exit.
        MeshToolApp::AutoShots shots;
        for (int i = 2; i < argc; i++)
        {
            if (!strcmp(argv[i], "--shots") && i + 1 < argc) shots.prefix = argv[++i];
            else if (!strcmp(argv[i], "--frame") && i + 1 < argc) shots.frameObject = argv[++i];
            else if (!strcmp(argv[i], "--yaw") && i + 1 < argc) shots.yawDeg = atof(argv[++i]);
            else if (!strcmp(argv[i], "--pitch") && i + 1 < argc) shots.pitchDeg = atof(argv[++i]);
            else if (!strcmp(argv[i], "--zoom") && i + 1 < argc) shots.zoom = atof(argv[++i]);
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
