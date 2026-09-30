#include "app.h"
#include <cstdio>
#include <exception>

int main(int argc, char* argv[])
{
    (void)argc;
    (void)argv;

    try
    {
        MeshToolApp app;
        if (!app.Init())
        {
            fprintf(stderr, "Init failed. Press enter to exit.\n");
            getchar();
            return 1;
        }

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
