#include "NewPipelineClientApp.h"

namespace wicked_newpipeline
{
void NewPipelineClientApp::CreateDebugUI()
{
    if (debug_ui_created)
        return;

    const NewPipelineSunState initial_sun = render_path.GetSunState();

    debug_window.Create(
        "NewPipeline Debug",
        wi::gui::Window::WindowControls::MOVE | wi::gui::Window::WindowControls::COLLAPSE);
    debug_window.SetPos(XMFLOAT2(20.0f, 80.0f));
    debug_window.SetSize(XMFLOAT2(340.0f, 355.0f));

    sun_enabled_checkbox.Create("Sun Enabled: ");
    sun_enabled_checkbox.SetPos(XMFLOAT2(150.0f, 8.0f));
    sun_enabled_checkbox.SetSize(XMFLOAT2(18.0f, 18.0f));
    sun_enabled_checkbox.SetCheck(initial_sun.enabled);
    sun_enabled_checkbox.OnClick([this](const wi::gui::EventArgs& args) {
        NewPipelineSunState state = render_path.GetSunState();
        state.enabled = args.bValue;
        render_path.SetSunState(state);
    });
    debug_window.AddWidget(&sun_enabled_checkbox);

    sun_yaw_slider.Create(-180.0f, 180.0f, initial_sun.yaw_degrees, 360.0f, "Sun Yaw: ");
    sun_yaw_slider.SetPos(XMFLOAT2(150.0f, 34.0f));
    sun_yaw_slider.SetSize(XMFLOAT2(145.0f, 18.0f));
    sun_yaw_slider.valueInputField.SetFloatPrecision(1);
    sun_yaw_slider.OnSlide([this](const wi::gui::EventArgs& args) {
        const NewPipelineSunState current = render_path.GetSunState();
        render_path.SetSunState(MakeSunStateFromAngles(current.enabled, args.fValue, current.pitch_degrees));
    });
    debug_window.AddWidget(&sun_yaw_slider);

    sun_pitch_slider.Create(-89.0f, 89.0f, initial_sun.pitch_degrees, 178.0f, "Sun Pitch: ");
    sun_pitch_slider.SetPos(XMFLOAT2(150.0f, 60.0f));
    sun_pitch_slider.SetSize(XMFLOAT2(145.0f, 18.0f));
    sun_pitch_slider.valueInputField.SetFloatPrecision(1);
    sun_pitch_slider.OnSlide([this](const wi::gui::EventArgs& args) {
        const NewPipelineSunState current = render_path.GetSunState();
        render_path.SetSunState(MakeSunStateFromAngles(current.enabled, current.yaw_degrees, args.fValue));
    });
    debug_window.AddWidget(&sun_pitch_slider);

    preview_buffer_combo.Create("Preview Buffer: ");
    preview_buffer_combo.SetPos(XMFLOAT2(150.0f, 88.0f));
    preview_buffer_combo.SetSize(XMFLOAT2(165.0f, 18.0f));
    preview_buffer_combo.AddItem("Final", static_cast<uint64_t>(DebugPreviewMode::Final));
    preview_buffer_combo.AddItem("Projected Depth", static_cast<uint64_t>(DebugPreviewMode::GBufferDepth));
    preview_buffer_combo.AddItem("Normal/Roughness", static_cast<uint64_t>(DebugPreviewMode::GBufferNormalRoughness));
    preview_buffer_combo.AddItem("Normal XY", static_cast<uint64_t>(DebugPreviewMode::GBufferNormalXY));
    preview_buffer_combo.AddItem("Roughness", static_cast<uint64_t>(DebugPreviewMode::GBufferRoughness));
    preview_buffer_combo.AddItem("Local Indirect Diffuse", static_cast<uint64_t>(DebugPreviewMode::LocalIndirectDiffuse));
    preview_buffer_combo.AddItem("Local AO", static_cast<uint64_t>(DebugPreviewMode::LocalAO));
    preview_buffer_combo.AddItem("Local Specular Indirect", static_cast<uint64_t>(DebugPreviewMode::LocalSpecularIndirect));
    preview_buffer_combo.AddItem("Local Shadow Visibility", static_cast<uint64_t>(DebugPreviewMode::LocalShadowVisibility));
    preview_buffer_combo.AddItem("Remote Indirect Diffuse", static_cast<uint64_t>(DebugPreviewMode::RemoteIndirectDiffuse));
    preview_buffer_combo.AddItem("Remote AO", static_cast<uint64_t>(DebugPreviewMode::RemoteAO));
    preview_buffer_combo.AddItem("Remote Specular Indirect", static_cast<uint64_t>(DebugPreviewMode::RemoteSpecularIndirect));
    preview_buffer_combo.AddItem("Remote Shadow Visibility", static_cast<uint64_t>(DebugPreviewMode::RemoteShadowVisibility));
    preview_buffer_combo.AddItem("Remote 2x2 Overview", static_cast<uint64_t>(DebugPreviewMode::RemoteOverview));
    preview_buffer_combo.SetSelectedByUserdataWithoutCallback(static_cast<uint64_t>(render_path.GetDebugPreviewMode()));
    preview_buffer_combo.OnSelect([this](const wi::gui::EventArgs& args) {
        render_path.SetDebugPreviewMode(static_cast<DebugPreviewMode>(args.userdata));
    });
    debug_window.AddWidget(&preview_buffer_combo);

    algorithm_label.Create("Algorithms");
    algorithm_label.SetText(render_path.GetEffectiveAlgorithmSummary());
    algorithm_label.SetPos(XMFLOAT2(10.0f, 116.0f));
    algorithm_label.SetSize(XMFLOAT2(310.0f, 38.0f));
    debug_window.AddWidget(&algorithm_label);

    const NewPipelineClientRenderSettings initial_render_settings = render_path.GetRenderSettings();

    shadow_maps_checkbox.Create("Local Shadows: ");
    shadow_maps_checkbox.SetPos(XMFLOAT2(150.0f, 158.0f));
    shadow_maps_checkbox.SetSize(XMFLOAT2(18.0f, 18.0f));
    shadow_maps_checkbox.SetCheck(initial_render_settings.shadow_maps_enabled);
    shadow_maps_checkbox.OnClick([this](const wi::gui::EventArgs& args) {
        NewPipelineClientRenderSettings settings = render_path.GetRenderSettings();
        settings.shadow_maps_enabled = args.bValue;
        render_path.SetRenderSettings(settings);
    });
    debug_window.AddWidget(&shadow_maps_checkbox);

    ssao_checkbox.Create("Local AO: ");
    ssao_checkbox.SetPos(XMFLOAT2(150.0f, 184.0f));
    ssao_checkbox.SetSize(XMFLOAT2(18.0f, 18.0f));
    ssao_checkbox.SetCheck(initial_render_settings.ssao_enabled);
    ssao_checkbox.OnClick([this](const wi::gui::EventArgs& args) {
        NewPipelineClientRenderSettings settings = render_path.GetRenderSettings();
        settings.ssao_enabled = args.bValue;
        render_path.SetRenderSettings(settings);
    });
    debug_window.AddWidget(&ssao_checkbox);

    environment_probe_checkbox.Create("Env Probe: ");
    environment_probe_checkbox.SetPos(XMFLOAT2(150.0f, 210.0f));
    environment_probe_checkbox.SetSize(XMFLOAT2(18.0f, 18.0f));
    environment_probe_checkbox.SetCheck(initial_render_settings.environment_probe_enabled);
    environment_probe_checkbox.OnClick([this](const wi::gui::EventArgs& args) {
        NewPipelineClientRenderSettings settings = render_path.GetRenderSettings();
        settings.environment_probe_enabled = args.bValue;
        render_path.SetRenderSettings(settings);
    });
    debug_window.AddWidget(&environment_probe_checkbox);

    baked_lightmaps_checkbox.Create("Use Baked Lightmaps: ");
    baked_lightmaps_checkbox.SetPos(XMFLOAT2(150.0f, 236.0f));
    baked_lightmaps_checkbox.SetSize(XMFLOAT2(18.0f, 18.0f));
    baked_lightmaps_checkbox.SetCheck(initial_render_settings.baked_lightmaps_enabled);
    baked_lightmaps_checkbox.OnClick([this](const wi::gui::EventArgs& args) {
        NewPipelineClientRenderSettings settings = render_path.GetRenderSettings();
        settings.baked_lightmaps_enabled = args.bValue;
        if (!settings.baked_lightmaps_enabled)
            settings.lightmap_bake_requested = false;
        render_path.SetRenderSettings(settings);
        bake_lightmaps_checkbox.SetCheck(settings.lightmap_bake_requested);
    });
    debug_window.AddWidget(&baked_lightmaps_checkbox);

    bake_lightmaps_checkbox.Create("Bake Lightmaps: ");
    bake_lightmaps_checkbox.SetPos(XMFLOAT2(150.0f, 262.0f));
    bake_lightmaps_checkbox.SetSize(XMFLOAT2(18.0f, 18.0f));
    bake_lightmaps_checkbox.SetCheck(initial_render_settings.lightmap_bake_requested);
    bake_lightmaps_checkbox.OnClick([this](const wi::gui::EventArgs& args) {
        NewPipelineClientRenderSettings settings = render_path.GetRenderSettings();
        settings.lightmap_bake_requested = args.bValue;
        if (settings.lightmap_bake_requested)
            settings.baked_lightmaps_enabled = true;
        render_path.SetRenderSettings(settings);
        baked_lightmaps_checkbox.SetCheck(settings.baked_lightmaps_enabled);
    });
    debug_window.AddWidget(&bake_lightmaps_checkbox);

    render_path.GetGUI().AddWidget(&debug_window);
    debug_ui_created = true;
}

void NewPipelineClientApp::ConfigureFromCommandLine(int argc, char* argv[])
{
    runtime_config = ParseRuntimeConfig(argc, argv, runtime_config);
    configured = true;
    render_path.SetRuntimeConfig(runtime_config);
}

void NewPipelineClientApp::Initialize()
{
    wi::Application::Initialize();
    // Application::Initialize() derives this from the optional command line
    // flag, so set the production default after the base implementation.
    alwaysactive = true;

    if (!configured)
        render_path.SetRuntimeConfig(runtime_config);

    for (const std::string& warning : runtime_config.parse_warnings)
    {
        wi::backlog::post("NewPipeline Client config warning: " + warning);
    }

    infoDisplay.active      = true;
    infoDisplay.watermark   = true;
    infoDisplay.resolution  = true;
    infoDisplay.fpsinfo     = true;
    infoDisplay.device_name = true;

    ActivatePath(&render_path);
}

void NewPipelineClientApp::Update(float dt)
{
    // Application::Update() is only reached after Wicked's asynchronous engine
    // initialization has completed. Creating GUI widgets earlier can enqueue
    // glyphs before the default font style exists and race UpdateAtlas().
    CreateDebugUI();
    render_path.SetInputActive(is_window_active);
    wi::Application::Update(dt);
}

std::string NewPipelineClientApp::GetWindowTitle() const
{
    return std::string{"NewPipeline_Wicked Client ["} + ToString(runtime_config.remote_source) + "]";
}
} // namespace wicked_newpipeline
