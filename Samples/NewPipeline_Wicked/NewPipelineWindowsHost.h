#pragma once

#if defined(_WIN32)
#include "WickedEngine.h"

#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1
#endif
#include <windows.h>

#include <string>
#include <vector>

namespace wicked_newpipeline
{
struct WindowsCommandLineArguments
{
    WindowsCommandLineArguments() = default;
    WindowsCommandLineArguments(const WindowsCommandLineArguments&) = delete;
    WindowsCommandLineArguments& operator=(const WindowsCommandLineArguments&) = delete;
    WindowsCommandLineArguments(WindowsCommandLineArguments&&) = default;
    WindowsCommandLineArguments& operator=(WindowsCommandLineArguments&&) = default;

    std::vector<std::string> values;
    std::vector<char*> pointers;

    int Count() const { return static_cast<int>(pointers.size()); }
    char** Data() { return pointers.empty() ? nullptr : pointers.data(); }
};

WindowsCommandLineArguments GetWindowsCommandLineArguments();

int RunWindowsApplication(
    wi::Application& application,
    HINSTANCE instance,
    const wchar_t* window_title,
    int show_command);
}
#endif
