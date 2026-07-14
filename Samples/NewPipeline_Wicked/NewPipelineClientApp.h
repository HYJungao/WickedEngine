#pragma once

#include "NewPipelineClientRenderPath.h"

#include "wiGUI.h"

#include <string>

namespace wicked_newpipeline
{
class NewPipelineClientApp final : public wi::Application
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
    NewPipelineClientRenderPath  render_path;
    wi::gui::Window              debug_window;
    wi::gui::CheckBox            sun_enabled_checkbox;
    wi::gui::Slider              sun_yaw_slider;
    wi::gui::Slider              sun_pitch_slider;
    wi::gui::ComboBox            preview_buffer_combo;
    wi::gui::Label               algorithm_label;
    wi::gui::CheckBox            shadow_maps_checkbox;
    wi::gui::CheckBox            ssao_checkbox;
    wi::gui::CheckBox            environment_probe_checkbox;
    wi::gui::CheckBox            baked_lightmaps_checkbox;
    wi::gui::Button              generate_lightmaps_button;
    wi::gui::Button              cancel_lightmaps_button;
    wi::gui::Label               lightmap_progress_label;
    bool                         configured = false;
    bool                         debug_ui_created = false;
};
} // namespace wicked_newpipeline
