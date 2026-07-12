#pragma once

#include "NewPipelineServerRenderPath.h"
#include "wiGUI.h"

#include <string>

namespace wicked_newpipeline
{
class NewPipelineServerApp final : public wi::Application
{
public:
    void ConfigureFromCommandLine(int argc, char* argv[]);

    void Initialize() override;
    void Update(float dt) override;

    const RuntimeConfig& GetRuntimeConfig() const { return runtime_config; }
    std::string GetWindowTitle() const;

private:
    void CreateDebugUI();

    RuntimeConfig                runtime_config;
    NewPipelineServerSettings    server_settings;
    NewPipelineServerRenderPath  render_path;
    wi::gui::Window              debug_window;
    wi::gui::ComboBox            preview_buffer_combo;
    wi::gui::Label               algorithm_label;
    bool                         configured = false;
    bool                         debug_ui_created = false;
};
} // namespace wicked_newpipeline
