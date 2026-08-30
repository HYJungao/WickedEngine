#include "NewPipelineClientApp.h"
#include "NewPipelineWindowsHost.h"

#if defined(_WIN32)
int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR command_line, int show_command)
{
    auto arguments = wicked_newpipeline::GetWindowsCommandLineArguments();
    wicked_newpipeline::NewPipelineClientApp application;
    wi::arguments::Parse(command_line);
    application.ConfigureFromCommandLine(arguments.Count(), arguments.Data());
    const std::string title_utf8 = application.GetWindowTitle();
    const std::wstring title(title_utf8.begin(), title_utf8.end());
    return wicked_newpipeline::RunWindowsApplication(application, instance, title.c_str(), show_command);
}
#endif
