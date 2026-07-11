#pragma once

#include "NewPipelineServerRenderPath.h"

#include <string>

namespace wicked_newpipeline
{
class NewPipelineServerApp final : public wi::Application
{
public:
    void ConfigureFromCommandLine(int argc, char* argv[]);

    void Initialize() override;

    const RuntimeConfig& GetRuntimeConfig() const { return runtime_config; }
    std::string GetWindowTitle() const;

private:
    RuntimeConfig                runtime_config;
    NewPipelineServerSettings    server_settings;
    NewPipelineServerRenderPath  render_path;
    bool                         configured = false;
};
} // namespace wicked_newpipeline
