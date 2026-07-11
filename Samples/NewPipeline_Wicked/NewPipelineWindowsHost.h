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

namespace wicked_newpipeline
{
int RunWindowsApplication(
    wi::Application& application,
    HINSTANCE instance,
    const wchar_t* window_title,
    int show_command);
}
#endif
