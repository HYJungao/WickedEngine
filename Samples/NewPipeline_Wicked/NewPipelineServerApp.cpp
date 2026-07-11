#include "NewPipelineServerApp.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>

namespace wicked_newpipeline
{
namespace
{
bool HasStartupArgument(int argc, char* argv[], const std::string& name)
{
    for (int i = 1; i < argc; ++i)
    {
        if (argv[i] == nullptr)
            continue;

        const std::string arg = argv[i];
        if (arg == name || arg == ("--" + name))
            return true;
    }
    return false;
}

std::string GetStartupArgumentValue(int argc, char* argv[], const std::string& name)
{
    const std::string dashed = "--" + name;
    const std::string inline_prefix = dashed + "=";
    for (int i = 1; i < argc; ++i)
    {
        if (argv[i] == nullptr)
            continue;

        const std::string arg = argv[i];
        if (arg.rfind(inline_prefix, 0) == 0)
            return arg.substr(inline_prefix.size());

        if ((arg == name || arg == dashed) && i + 1 < argc && argv[i + 1] != nullptr)
            return argv[i + 1];
    }
    return {};
}

bool TryParseFloat(const std::string& value, float& result)
{
    if (value.empty())
        return false;

    char* end = nullptr;
    errno = 0;
    const float parsed = std::strtof(value.c_str(), &end);
    if (errno != 0 || end == value.c_str() || (end != nullptr && *end != '\0') || !std::isfinite(parsed))
        return false;

    result = parsed;
    return true;
}

bool TryParseUInt32(const std::string& value, uint32_t& result)
{
    if (value.empty())
        return false;

    char* end = nullptr;
    errno = 0;
    const unsigned long parsed = std::strtoul(value.c_str(), &end, 10);
    if (errno != 0 || end == value.c_str() || (end != nullptr && *end != '\0'))
        return false;

    result = static_cast<uint32_t>(std::min<unsigned long>(parsed, 65535ul));
    return true;
}
} // namespace

void NewPipelineServerApp::ConfigureFromCommandLine(int argc, char* argv[])
{
    runtime_config = ParseRuntimeConfig(argc, argv, runtime_config);
    server_settings.ddgi_enabled = !HasStartupArgument(argc, argv, "no_ddgi");
    server_settings.ddgi_debug_formal = HasStartupArgument(argc, argv, "ddgi_debug_formal");

    std::string remote_fps_arg = GetStartupArgumentValue(argc, argv, "remote_fps");
    const bool used_legacy_remote_fps = remote_fps_arg.empty();
    if (used_legacy_remote_fps)
        remote_fps_arg = GetStartupArgumentValue(argc, argv, "mock_remote_fps");
    if (!remote_fps_arg.empty())
    {
        float parsed = 0.0f;
        if (TryParseFloat(remote_fps_arg, parsed))
        {
            server_settings.remote_publish_fps = std::clamp(parsed, 0.0f, 30.0f);
            if (used_legacy_remote_fps)
                runtime_config.parse_warnings.push_back("--mock_remote_fps is deprecated; use --remote_fps.");
        }
        else
        {
            runtime_config.parse_warnings.push_back(
                "Unknown remote FPS value '" + remote_fps_arg + "', using " +
                std::to_string(server_settings.remote_publish_fps));
        }
    }

    const std::string ddgi_rays_arg = GetStartupArgumentValue(argc, argv, "ddgi_rays");
    if (!ddgi_rays_arg.empty())
    {
        uint32_t parsed = 0;
        if (TryParseUInt32(ddgi_rays_arg, parsed))
        {
            server_settings.ddgi_ray_count = std::clamp(parsed, 0u, 2048u);
        }
        else
        {
            runtime_config.parse_warnings.push_back(
                "Unknown --ddgi_rays value '" + ddgi_rays_arg + "', using " +
                std::to_string(server_settings.ddgi_ray_count));
        }
    }

    configured = true;
    render_path.SetRuntimeConfig(runtime_config);
    render_path.SetServerSettings(server_settings);
}

void NewPipelineServerApp::Initialize()
{
    wi::Application::Initialize();

    if (!configured)
    {
        render_path.SetRuntimeConfig(runtime_config);
        render_path.SetServerSettings(server_settings);
    }

    for (const std::string& warning : runtime_config.parse_warnings)
    {
        wi::backlog::post("NewPipeline Server config warning: " + warning);
    }

    infoDisplay.active      = true;
    infoDisplay.watermark   = true;
    infoDisplay.resolution  = true;
    infoDisplay.fpsinfo     = true;
    infoDisplay.device_name = true;

    ActivatePath(&render_path);
}

std::string NewPipelineServerApp::GetWindowTitle() const
{
    return std::string{"NewPipeline_Wicked Server ["} + ToString(runtime_config.remote_source) + "]";
}
} // namespace wicked_newpipeline
