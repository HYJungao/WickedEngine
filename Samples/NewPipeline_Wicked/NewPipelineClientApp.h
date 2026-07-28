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
    wi::gui::Window              elastic_lighting_window;
    wi::gui::CheckBox            sun_enabled_checkbox;
    wi::gui::Slider              sun_yaw_slider;
    wi::gui::Slider              sun_pitch_slider;
    wi::gui::ComboBox            preview_buffer_combo;
    wi::gui::Label               algorithm_label;
    wi::gui::CheckBox            shadow_maps_checkbox;
    wi::gui::CheckBox            ssao_checkbox;
    wi::gui::CheckBox            environment_probe_checkbox;
    wi::gui::CheckBox            baked_lightmaps_checkbox;
    wi::gui::Button              generate_static_lighting_button;
    wi::gui::Label               static_lighting_progress_label;
    wi::gui::Button              generate_lightmaps_button;
    wi::gui::Button              cancel_lightmaps_button;
    wi::gui::Label               lightmap_progress_label;
    wi::gui::Button              generate_reflection_probe_button;
    wi::gui::Slider              reflection_probe_mip_slider;
    wi::gui::Label               reflection_probe_progress_label;
    wi::gui::CheckBox            remote_gi_checkbox;
    wi::gui::Slider              remote_gi_weight_slider;
    wi::gui::CheckBox            remote_ao_checkbox;
    wi::gui::Slider              remote_ao_weight_slider;
    wi::gui::CheckBox            remote_specular_checkbox;
    wi::gui::Slider              remote_specular_weight_slider;
    wi::gui::CheckBox            remote_shadow_checkbox;
    wi::gui::Slider              remote_shadow_weight_slider;
    wi::gui::Label               elastic_lighting_status_label;
    bool                         configured = false;
    bool                         debug_ui_created = false;
};
} // namespace wicked_newpipeline
