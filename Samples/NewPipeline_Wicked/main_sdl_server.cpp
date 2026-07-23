#include "NewPipelineServerApp.h"

#include <SDL2/SDL.h>

wicked_newpipeline::NewPipelineServerApp application;

int main(int argc, char* argv[])
{
    for (int i = 1; i < argc; ++i)
    {
        if (std::string{argv[i]} == "--transport_selftest")
        {
            std::string error;
            return wicked_newpipeline::ValidateRemoteTransportSelfTest(&error) ? 0 : 1;
        }
    }
    wi::arguments::Parse(argc, argv);
    application.ConfigureFromCommandLine(argc, argv);

    sdl2::sdlsystem_ptr_t system = sdl2::make_sdlsystem(SDL_INIT_EVERYTHING | SDL_INIT_EVENTS);
    sdl2::window_ptr_t window = sdl2::make_window(
        application.GetWindowTitle().c_str(),
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        1920,
        1080,
        SDL_WINDOW_SHOWN | SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);

    application.SetWindow(window.get());

    bool quit = false;
    while (!quit)
    {
        SDL_PumpEvents();
        application.Run();

        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            switch (event.type)
            {
            case SDL_QUIT:
                quit = true;
                break;
            case SDL_WINDOWEVENT:
                switch (event.window.event)
                {
                case SDL_WINDOWEVENT_CLOSE:
                    quit = true;
                    break;
                case SDL_WINDOWEVENT_RESIZED:
                    application.SetWindow(application.window);
                    break;
                default:
                    break;
                }
                break;
            default:
                break;
            }
            wi::input::sdlinput::ProcessEvent(event);
        }
    }

    wi::jobsystem::ShutDown();
    SDL_Quit();

    return 0;
}
