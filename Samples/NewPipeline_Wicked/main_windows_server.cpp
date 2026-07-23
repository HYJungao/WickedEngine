#include "NewPipelineServerApp.h"
#include "NewPipelineWindowsHost.h"

#if defined(_WIN32)
int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR command_line, int show_command)
{
    for (int i = 1; i < __argc; ++i)
    {
        if (std::string{__argv[i]} == "--transport_selftest")
        {
            std::string error;
            return wicked_newpipeline::ValidateRemoteTransportSelfTest(&error) ? 0 : 1;
        }
    }
    wicked_newpipeline::NewPipelineServerApp application;
    wi::arguments::Parse(command_line);
    application.ConfigureFromCommandLine(__argc, __argv);
    const std::string title_utf8 = application.GetWindowTitle();
    const std::wstring title(title_utf8.begin(), title_utf8.end());
    return wicked_newpipeline::RunWindowsApplication(application, instance, title.c_str(), show_command);
}
#endif
