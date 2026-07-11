#include "NewPipelineWindowsHost.h"

#if defined(_WIN32)
namespace wicked_newpipeline
{
namespace
{
wi::Application* active_application = nullptr;

LRESULT CALLBACK NewPipelineWindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message)
    {
    case WM_SIZE:
    case WM_DPICHANGED:
        if (active_application && active_application->is_window_active)
            active_application->SetWindow(window);
        return 0;
    case WM_CHAR:
        if (wparam == VK_BACK)
            wi::gui::TextInputField::DeleteFromInput();
        else if (wparam != VK_RETURN)
            wi::gui::TextInputField::AddInput(static_cast<wchar_t>(wparam));
        return 0;
    case WM_INPUT:
        wi::input::rawinput::ParseMessage(reinterpret_cast<void*>(lparam));
        return 0;
    case WM_KILLFOCUS:
        if (active_application)
            active_application->is_window_active = false;
        return 0;
    case WM_SETFOCUS:
        if (active_application)
            active_application->is_window_active = true;
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}
} // namespace

int RunWindowsApplication(
    wi::Application& application,
    HINSTANCE instance,
    const wchar_t* window_title,
    int show_command)
{
    active_application = &application;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    constexpr wchar_t class_name[] = L"NewPipelineWickedWindow";
    WNDCLASSEXW window_class = {};
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = NewPipelineWindowProc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    window_class.lpszClassName = class_name;
    if (!RegisterClassExW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return 2;

    RECT rect = {0, 0, 1280, 720};
    AdjustWindowRectEx(&rect, WS_OVERLAPPEDWINDOW, FALSE, 0);
    HWND window = CreateWindowExW(0, class_name, window_title, WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top,
        nullptr, nullptr, instance, nullptr);
    if (!window)
        return 3;

    ShowWindow(window, show_command == 0 ? SW_SHOWDEFAULT : show_command);
    UpdateWindow(window);
    application.SetWindow(window);

    MSG message = {};
    while (message.message != WM_QUIT)
    {
        if (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        else
        {
            application.Run();
        }
    }

    active_application = nullptr;
    wi::jobsystem::ShutDown();
    return static_cast<int>(message.wParam);
}
} // namespace wicked_newpipeline
#endif
