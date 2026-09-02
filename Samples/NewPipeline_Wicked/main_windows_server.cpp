#include "NewPipelineServerApp.h"
#include "NewPipelineWindowsHost.h"

#include <cstdio>

#if defined(_WIN32)
int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR command_line, int show_command)
{
    auto arguments = wicked_newpipeline::GetWindowsCommandLineArguments();
    for (int i = 1; i < arguments.Count(); ++i)
    {
        if (std::string{arguments.Data()[i]} == "--transport_selftest")
        {
            std::string error;
            if (wicked_newpipeline::ValidateRemoteTransportSelfTest(&error))
                return 0;
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
    }
    wicked_newpipeline::NewPipelineServerApp application;
    wi::arguments::Parse(command_line);
    application.ConfigureFromCommandLine(arguments.Count(), arguments.Data());
    const std::string title_utf8 = application.GetWindowTitle();
    const std::wstring title(title_utf8.begin(), title_utf8.end());
    return wicked_newpipeline::RunWindowsApplication(application, instance, title.c_str(), show_command);
}
#endif
