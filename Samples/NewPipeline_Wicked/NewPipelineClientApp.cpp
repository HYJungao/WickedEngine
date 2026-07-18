#include "NewPipelineClientApp.h"

#include <algorithm>
#include <cmath>

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
    debug_window.SetSize(XMFLOAT2(340.0f, 675.0f));

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
    preview_buffer_combo.AddItem("Local Lightmap Irradiance", static_cast<uint64_t>(DebugPreviewMode::LocalIndirectDiffuse));
    preview_buffer_combo.AddItem("Local Lightmap Validity", static_cast<uint64_t>(DebugPreviewMode::LocalLightmapValidity));
    preview_buffer_combo.AddItem("Local Lightmap Coverage", static_cast<uint64_t>(DebugPreviewMode::LocalLightmapCoverage));
    preview_buffer_combo.AddItem("Local Lightmap Raw", static_cast<uint64_t>(DebugPreviewMode::LocalLightmapRaw));
    preview_buffer_combo.AddItem("Local Indirect (Final Input)", static_cast<uint64_t>(DebugPreviewMode::LocalIndirectFinalInput));
    preview_buffer_combo.AddItem("Local AO", static_cast<uint64_t>(DebugPreviewMode::LocalAO));
    preview_buffer_combo.AddItem("Local Probe Specular", static_cast<uint64_t>(DebugPreviewMode::LocalSpecularIndirect));
    preview_buffer_combo.AddItem("Local Probe Cubemap", static_cast<uint64_t>(DebugPreviewMode::LocalReflectionProbe));
    preview_buffer_combo.AddItem("Local Shadow Map (atlas only)", static_cast<uint64_t>(DebugPreviewMode::LocalShadowVisibility));
    preview_buffer_combo.AddItem("Remote Indirect Diffuse", static_cast<uint64_t>(DebugPreviewMode::RemoteIndirectDiffuse));
    preview_buffer_combo.AddItem("Remote AO", static_cast<uint64_t>(DebugPreviewMode::RemoteAO));
    preview_buffer_combo.AddItem("Remote Specular Indirect", static_cast<uint64_t>(DebugPreviewMode::RemoteSpecularIndirect));
    preview_buffer_combo.AddItem("Remote Shadow Visibility", static_cast<uint64_t>(DebugPreviewMode::RemoteShadowVisibility));
    preview_buffer_combo.AddItem("Elastic GI (Final Input)", static_cast<uint64_t>(DebugPreviewMode::ElasticIndirectDiffuse));
    preview_buffer_combo.AddItem("Elastic AO (Final Input)", static_cast<uint64_t>(DebugPreviewMode::ElasticAO));
    preview_buffer_combo.AddItem("Remote 2x2 Overview", static_cast<uint64_t>(DebugPreviewMode::RemoteOverview));
    preview_buffer_combo.SetSelectedByUserdataWithoutCallback(static_cast<uint64_t>(render_path.GetDebugPreviewMode()));
    preview_buffer_combo.OnSelect([this](const wi::gui::EventArgs& args) {
        render_path.SetDebugPreviewMode(static_cast<DebugPreviewMode>(args.userdata));
    });
    debug_window.AddWidget(&preview_buffer_combo);

    algorithm_label.Create("Algorithms");
    algorithm_label.SetText(render_path.GetDebugStatusSummary());
    algorithm_label.SetPos(XMFLOAT2(10.0f, 116.0f));
    algorithm_label.SetSize(XMFLOAT2(310.0f, 110.0f));
    debug_window.AddWidget(&algorithm_label);

    const NewPipelineClientRenderSettings initial_render_settings = render_path.GetRenderSettings();

    shadow_maps_checkbox.Create("Local Shadows: ");
    shadow_maps_checkbox.SetPos(XMFLOAT2(150.0f, 233.0f));
    shadow_maps_checkbox.SetSize(XMFLOAT2(18.0f, 18.0f));
    shadow_maps_checkbox.SetCheck(initial_render_settings.shadow_maps_enabled);
    shadow_maps_checkbox.OnClick([this](const wi::gui::EventArgs& args) {
        NewPipelineClientRenderSettings settings = render_path.GetRenderSettings();
        settings.shadow_maps_enabled = args.bValue;
        render_path.SetRenderSettings(settings);
    });
    debug_window.AddWidget(&shadow_maps_checkbox);

    ssao_checkbox.Create("Local AO: ");
    ssao_checkbox.SetPos(XMFLOAT2(150.0f, 259.0f));
    ssao_checkbox.SetSize(XMFLOAT2(18.0f, 18.0f));
    ssao_checkbox.SetCheck(initial_render_settings.ssao_enabled);
    ssao_checkbox.OnClick([this](const wi::gui::EventArgs& args) {
        NewPipelineClientRenderSettings settings = render_path.GetRenderSettings();
        settings.ssao_enabled = args.bValue;
        render_path.SetRenderSettings(settings);
    });
    debug_window.AddWidget(&ssao_checkbox);

    environment_probe_checkbox.Create("Env Probe: ");
    environment_probe_checkbox.SetPos(XMFLOAT2(150.0f, 285.0f));
    environment_probe_checkbox.SetSize(XMFLOAT2(18.0f, 18.0f));
    environment_probe_checkbox.SetCheck(initial_render_settings.environment_probe_enabled);
    environment_probe_checkbox.OnClick([this](const wi::gui::EventArgs& args) {
        NewPipelineClientRenderSettings settings = render_path.GetRenderSettings();
        settings.environment_probe_enabled = args.bValue;
        render_path.SetRenderSettings(settings);
    });
    debug_window.AddWidget(&environment_probe_checkbox);

    baked_lightmaps_checkbox.Create("Use Baked Lightmaps: ");
    baked_lightmaps_checkbox.SetPos(XMFLOAT2(150.0f, 311.0f));
    baked_lightmaps_checkbox.SetSize(XMFLOAT2(18.0f, 18.0f));
    baked_lightmaps_checkbox.SetCheck(initial_render_settings.baked_lightmaps_enabled);
    baked_lightmaps_checkbox.OnClick([this](const wi::gui::EventArgs& args) {
        NewPipelineClientRenderSettings settings = render_path.GetRenderSettings();
        settings.baked_lightmaps_enabled = args.bValue;
        if (!settings.baked_lightmaps_enabled)
        {
            settings.lightmap_bake_requested = false;
            render_path.CancelLightmapBake();
        }
        render_path.SetRenderSettings(settings);
    });
    debug_window.AddWidget(&baked_lightmaps_checkbox);

    generate_static_lighting_button.Create("Generate Client Lighting");
    generate_static_lighting_button.SetTooltip(
        "Recommended: persist probe placement, bake Lightmaps, then capture the validated Reflection Probe package.");
    generate_static_lighting_button.SetPos(XMFLOAT2(10.0f, 341.0f));
    generate_static_lighting_button.SetSize(XMFLOAT2(310.0f, 24.0f));
    generate_static_lighting_button.OnClick([this](const wi::gui::EventArgs&) {
        render_path.RequestStaticLightingBake();
        baked_lightmaps_checkbox.SetCheck(true);
        environment_probe_checkbox.SetCheck(true);
    });
    debug_window.AddWidget(&generate_static_lighting_button);

    static_lighting_progress_label.Create("Client Lighting Status");
    static_lighting_progress_label.SetText(render_path.GetStaticLightingBakeStatus());
    static_lighting_progress_label.SetPos(XMFLOAT2(10.0f, 371.0f));
    static_lighting_progress_label.SetSize(XMFLOAT2(310.0f, 40.0f));
    debug_window.AddWidget(&static_lighting_progress_label);

    generate_lightmaps_button.Create("Generate Lightmap Only");
    generate_lightmaps_button.SetTooltip("Generate missing atlas UVs, update the single source .wiscene, and bake a sibling .clientlightmap package.");
    generate_lightmaps_button.SetPos(XMFLOAT2(10.0f, 417.0f));
    generate_lightmaps_button.SetSize(XMFLOAT2(195.0f, 24.0f));
    generate_lightmaps_button.OnClick([this](const wi::gui::EventArgs&) {
        render_path.RequestLightmapBake();
        baked_lightmaps_checkbox.SetCheck(true);
    });
    debug_window.AddWidget(&generate_lightmaps_button);

    cancel_lightmaps_button.Create("Cancel Bake");
    cancel_lightmaps_button.SetTooltip("Cancel the active Client Lighting bake without replacing previous packages.");
    cancel_lightmaps_button.SetPos(XMFLOAT2(215.0f, 417.0f));
    cancel_lightmaps_button.SetSize(XMFLOAT2(105.0f, 24.0f));
    cancel_lightmaps_button.OnClick([this](const wi::gui::EventArgs&) {
        render_path.CancelStaticLightingBake();
    });
    debug_window.AddWidget(&cancel_lightmaps_button);

    lightmap_progress_label.Create("Lightmap Progress");
    lightmap_progress_label.SetText(render_path.GetLightmapBakeStatus());
    lightmap_progress_label.SetPos(XMFLOAT2(10.0f, 448.0f));
    lightmap_progress_label.SetSize(XMFLOAT2(310.0f, 70.0f));
    debug_window.AddWidget(&lightmap_progress_label);

    generate_reflection_probe_button.Create("Generate Reflection Probe");
    generate_reflection_probe_button.SetTooltip(
        "Capture a validated sibling .clientprobe package. Probe placement must already be persisted by Generate Client Lighting.");
    generate_reflection_probe_button.SetPos(XMFLOAT2(10.0f, 524.0f));
    generate_reflection_probe_button.SetSize(XMFLOAT2(310.0f, 24.0f));
    generate_reflection_probe_button.OnClick([this](const wi::gui::EventArgs&) {
        render_path.RequestReflectionProbeBake();
        environment_probe_checkbox.SetCheck(true);
    });
    debug_window.AddWidget(&generate_reflection_probe_button);

    reflection_probe_mip_slider.Create(0.0f, 6.0f, 0.0f, 6.0f, "Probe Mip: ");
    reflection_probe_mip_slider.SetTooltip("Preview the BC6H prefiltered cubemap mip: 0 is sharp; higher mips represent rougher reflections.");
    reflection_probe_mip_slider.SetPos(XMFLOAT2(150.0f, 556.0f));
    reflection_probe_mip_slider.SetSize(XMFLOAT2(145.0f, 18.0f));
    reflection_probe_mip_slider.valueInputField.SetFloatPrecision(0);
    reflection_probe_mip_slider.OnSlide([this](const wi::gui::EventArgs& args) {
        render_path.SetReflectionProbeDebugMip(static_cast<uint32_t>(std::max(0.0f, std::round(args.fValue))));
    });
    debug_window.AddWidget(&reflection_probe_mip_slider);

    reflection_probe_progress_label.Create("Reflection Probe Progress");
    reflection_probe_progress_label.SetText(render_path.GetReflectionProbeBakeStatus());
    reflection_probe_progress_label.SetPos(XMFLOAT2(10.0f, 586.0f));
    reflection_probe_progress_label.SetSize(XMFLOAT2(310.0f, 55.0f));
    debug_window.AddWidget(&reflection_probe_progress_label);

    elastic_lighting_window.Create(
        "Elastic GI / AO",
        wi::gui::Window::WindowControls::MOVE | wi::gui::Window::WindowControls::COLLAPSE);
    elastic_lighting_window.SetPos(XMFLOAT2(375.0f, 80.0f));
    elastic_lighting_window.SetSize(XMFLOAT2(340.0f, 190.0f));

    remote_gi_checkbox.Create("Remote DDGI: ");
    remote_gi_checkbox.SetPos(XMFLOAT2(150.0f, 10.0f));
    remote_gi_checkbox.SetSize(XMFLOAT2(18.0f, 18.0f));
    remote_gi_checkbox.SetCheck(initial_render_settings.remote_gi_enabled);
    remote_gi_checkbox.OnClick([this](const wi::gui::EventArgs& args) {
        NewPipelineClientRenderSettings settings = render_path.GetRenderSettings();
        settings.remote_gi_enabled = args.bValue;
        render_path.SetRenderSettings(settings);
    });
    elastic_lighting_window.AddWidget(&remote_gi_checkbox);

    remote_gi_weight_slider.Create(0.0f, 1.0f, initial_render_settings.remote_gi_max_weight, 100.0f, "GI Remote Max: ");
    remote_gi_weight_slider.SetPos(XMFLOAT2(150.0f, 38.0f));
    remote_gi_weight_slider.SetSize(XMFLOAT2(150.0f, 18.0f));
    remote_gi_weight_slider.valueInputField.SetFloatPrecision(2);
    remote_gi_weight_slider.OnSlide([this](const wi::gui::EventArgs& args) {
        NewPipelineClientRenderSettings settings = render_path.GetRenderSettings();
        settings.remote_gi_max_weight = args.fValue;
        render_path.SetRenderSettings(settings);
    });
    elastic_lighting_window.AddWidget(&remote_gi_weight_slider);

    remote_ao_checkbox.Create("Remote RTAO: ");
    remote_ao_checkbox.SetPos(XMFLOAT2(150.0f, 68.0f));
    remote_ao_checkbox.SetSize(XMFLOAT2(18.0f, 18.0f));
    remote_ao_checkbox.SetCheck(initial_render_settings.remote_ao_enabled);
    remote_ao_checkbox.OnClick([this](const wi::gui::EventArgs& args) {
        NewPipelineClientRenderSettings settings = render_path.GetRenderSettings();
        settings.remote_ao_enabled = args.bValue;
        render_path.SetRenderSettings(settings);
    });
    elastic_lighting_window.AddWidget(&remote_ao_checkbox);

    remote_ao_weight_slider.Create(0.0f, 1.0f, initial_render_settings.remote_ao_max_weight, 100.0f, "AO Remote Max: ");
    remote_ao_weight_slider.SetPos(XMFLOAT2(150.0f, 96.0f));
    remote_ao_weight_slider.SetSize(XMFLOAT2(150.0f, 18.0f));
    remote_ao_weight_slider.valueInputField.SetFloatPrecision(2);
    remote_ao_weight_slider.OnSlide([this](const wi::gui::EventArgs& args) {
        NewPipelineClientRenderSettings settings = render_path.GetRenderSettings();
        settings.remote_ao_max_weight = args.fValue;
        render_path.SetRenderSettings(settings);
    });
    elastic_lighting_window.AddWidget(&remote_ao_weight_slider);

    elastic_lighting_status_label.Create("Elastic Lighting Status");
    elastic_lighting_status_label.SetText(render_path.GetElasticLightingStatus());
    elastic_lighting_status_label.SetPos(XMFLOAT2(10.0f, 126.0f));
    elastic_lighting_status_label.SetSize(XMFLOAT2(310.0f, 45.0f));
    elastic_lighting_window.AddWidget(&elastic_lighting_status_label);

    const LightmapDiagnosticSettings initial_diagnostic_settings =
        render_path.GetLightmapDiagnosticSettings();
    lightmap_diagnostic_window.Create(
        "Lightmap Diagnostics",
        wi::gui::Window::WindowControls::MOVE | wi::gui::Window::WindowControls::COLLAPSE);
    lightmap_diagnostic_window.SetPos(XMFLOAT2(720.0f, 80.0f));
    lightmap_diagnostic_window.SetSize(XMFLOAT2(390.0f, 345.0f));

    diagnostic_pick_center_button.Create("Pick Object at Screen Center");
    diagnostic_pick_center_button.SetTooltip(
        "Aim the camera center at the artifact mesh, then click. Diagnostic bakes affect only this object.");
    diagnostic_pick_center_button.SetPos(XMFLOAT2(10.0f, 10.0f));
    diagnostic_pick_center_button.SetSize(XMFLOAT2(360.0f, 24.0f));
    diagnostic_pick_center_button.OnClick([this](const wi::gui::EventArgs&) {
        render_path.PickLightmapDiagnosticObjectAtScreenCenter();
    });
    lightmap_diagnostic_window.AddWidget(&diagnostic_pick_center_button);

    diagnostic_resolution_combo.Create("Target Long Edge: ");
    diagnostic_resolution_combo.SetPos(XMFLOAT2(175.0f, 44.0f));
    diagnostic_resolution_combo.SetSize(XMFLOAT2(190.0f, 20.0f));
    for (uint32_t value : {128u, 256u, 512u, 1024u})
        diagnostic_resolution_combo.AddItem(std::to_string(value), value);
    diagnostic_resolution_combo.SetSelectedByUserdataWithoutCallback(initial_diagnostic_settings.resolution);
    diagnostic_resolution_combo.OnSelect([this](const wi::gui::EventArgs& args) {
        LightmapDiagnosticSettings settings = render_path.GetLightmapDiagnosticSettings();
        settings.resolution = static_cast<uint32_t>(args.userdata);
        render_path.SetLightmapDiagnosticSettings(settings);
    });
    lightmap_diagnostic_window.AddWidget(&diagnostic_resolution_combo);

    diagnostic_samples_combo.Create("Samples: ");
    diagnostic_samples_combo.SetPos(XMFLOAT2(175.0f, 74.0f));
    diagnostic_samples_combo.SetSize(XMFLOAT2(190.0f, 20.0f));
    for (uint32_t value : {128u, 512u, 2048u})
        diagnostic_samples_combo.AddItem(std::to_string(value), value);
    diagnostic_samples_combo.SetSelectedByUserdataWithoutCallback(initial_diagnostic_settings.sample_count);
    diagnostic_samples_combo.OnSelect([this](const wi::gui::EventArgs& args) {
        LightmapDiagnosticSettings settings = render_path.GetLightmapDiagnosticSettings();
        settings.sample_count = static_cast<uint32_t>(args.userdata);
        render_path.SetLightmapDiagnosticSettings(settings);
    });
    lightmap_diagnostic_window.AddWidget(&diagnostic_samples_combo);

    diagnostic_bounces_combo.Create("Bounces: ");
    diagnostic_bounces_combo.SetPos(XMFLOAT2(175.0f, 104.0f));
    diagnostic_bounces_combo.SetSize(XMFLOAT2(190.0f, 20.0f));
    diagnostic_bounces_combo.AddItem("1 (direct + sky)", 1);
    diagnostic_bounces_combo.AddItem("2 (first indirect)", 2);
    diagnostic_bounces_combo.AddItem("3 (full)", 3);
    diagnostic_bounces_combo.SetSelectedByUserdataWithoutCallback(initial_diagnostic_settings.bounce_count);
    diagnostic_bounces_combo.OnSelect([this](const wi::gui::EventArgs& args) {
        LightmapDiagnosticSettings settings = render_path.GetLightmapDiagnosticSettings();
        settings.bounce_count = static_cast<uint32_t>(args.userdata);
        render_path.SetLightmapDiagnosticSettings(settings);
    });
    lightmap_diagnostic_window.AddWidget(&diagnostic_bounces_combo);

    diagnostic_regenerate_atlas_checkbox.Create("Force Regenerate Atlas: ");
    diagnostic_regenerate_atlas_checkbox.SetPos(XMFLOAT2(175.0f, 136.0f));
    diagnostic_regenerate_atlas_checkbox.SetSize(XMFLOAT2(18.0f, 18.0f));
    diagnostic_regenerate_atlas_checkbox.SetCheck(initial_diagnostic_settings.force_regenerate_atlas);
    diagnostic_regenerate_atlas_checkbox.OnClick([this](const wi::gui::EventArgs& args) {
        LightmapDiagnosticSettings settings = render_path.GetLightmapDiagnosticSettings();
        settings.force_regenerate_atlas = args.bValue;
        render_path.SetLightmapDiagnosticSettings(settings);
    });
    lightmap_diagnostic_window.AddWidget(&diagnostic_regenerate_atlas_checkbox);

    diagnostic_pause_before_save_checkbox.Create("Pause Before Save: ");
    diagnostic_pause_before_save_checkbox.SetPos(XMFLOAT2(175.0f, 164.0f));
    diagnostic_pause_before_save_checkbox.SetSize(XMFLOAT2(18.0f, 18.0f));
    diagnostic_pause_before_save_checkbox.SetCheck(initial_diagnostic_settings.pause_before_save);
    diagnostic_pause_before_save_checkbox.OnClick([this](const wi::gui::EventArgs& args) {
        LightmapDiagnosticSettings settings = render_path.GetLightmapDiagnosticSettings();
        settings.pause_before_save = args.bValue;
        render_path.SetLightmapDiagnosticSettings(settings);
    });
    lightmap_diagnostic_window.AddWidget(&diagnostic_pause_before_save_checkbox);

    diagnostic_bake_button.Create("Bake Selected Diagnostic Object");
    diagnostic_bake_button.SetPos(XMFLOAT2(10.0f, 194.0f));
    diagnostic_bake_button.SetSize(XMFLOAT2(360.0f, 24.0f));
    diagnostic_bake_button.OnClick([this](const wi::gui::EventArgs&) {
        render_path.RequestLightmapDiagnosticBake();
    });
    lightmap_diagnostic_window.AddWidget(&diagnostic_bake_button);

    diagnostic_resume_button.Create("Resume: Temporary Save Pipeline");
    diagnostic_resume_button.SetTooltip(
        "Runs SaveLightmap (optional OIDN, then BC6H) in memory. Production sidecars are not replaced.");
    diagnostic_resume_button.SetPos(XMFLOAT2(10.0f, 224.0f));
    diagnostic_resume_button.SetSize(XMFLOAT2(230.0f, 24.0f));
    diagnostic_resume_button.OnClick([this](const wi::gui::EventArgs&) {
        render_path.ResumeLightmapDiagnosticSave();
    });
    lightmap_diagnostic_window.AddWidget(&diagnostic_resume_button);

    diagnostic_discard_button.Create("Discard / Restore");
    diagnostic_discard_button.SetPos(XMFLOAT2(250.0f, 224.0f));
    diagnostic_discard_button.SetSize(XMFLOAT2(120.0f, 24.0f));
    diagnostic_discard_button.OnClick([this](const wi::gui::EventArgs&) {
        render_path.DiscardLightmapDiagnosticBake();
    });
    lightmap_diagnostic_window.AddWidget(&diagnostic_discard_button);

    diagnostic_status_label.Create("Lightmap Diagnostic Status");
    diagnostic_status_label.SetText(render_path.GetLightmapDiagnosticStatus());
    diagnostic_status_label.SetPos(XMFLOAT2(10.0f, 258.0f));
    diagnostic_status_label.SetSize(XMFLOAT2(360.0f, 65.0f));
    lightmap_diagnostic_window.AddWidget(&diagnostic_status_label);

    render_path.GetGUI().AddWidget(&debug_window);
    render_path.GetGUI().AddWidget(&elastic_lighting_window);
    render_path.GetGUI().AddWidget(&lightmap_diagnostic_window);
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
    if (debug_ui_created)
    {
        algorithm_label.SetText(render_path.GetDebugStatusSummary());
        static_lighting_progress_label.SetText(render_path.GetStaticLightingBakeStatus());
        lightmap_progress_label.SetText(render_path.GetLightmapBakeStatus());
        reflection_probe_progress_label.SetText(render_path.GetReflectionProbeBakeStatus());
        elastic_lighting_status_label.SetText(render_path.GetElasticLightingStatus());
        diagnostic_status_label.SetText(render_path.GetLightmapDiagnosticStatus());
        const bool lightmap_active = render_path.IsLightmapBakeActive();
        const bool probe_active = render_path.IsReflectionProbeBakeActive();
        const bool static_lighting_active = render_path.IsStaticLightingBakeActive();
        const bool diagnostic_active = render_path.IsLightmapDiagnosticSessionActive();
        const bool diagnostic_controls_enabled = !lightmap_active && !probe_active;
        generate_static_lighting_button.SetEnabled(!static_lighting_active);
        generate_lightmaps_button.SetEnabled(!lightmap_active && !probe_active);
        cancel_lightmaps_button.SetEnabled(static_lighting_active);
        generate_reflection_probe_button.SetEnabled(!lightmap_active && !probe_active);
        sun_enabled_checkbox.SetEnabled(!static_lighting_active);
        sun_yaw_slider.SetEnabled(!static_lighting_active);
        sun_pitch_slider.SetEnabled(!static_lighting_active);
        const uint32_t mip_count = render_path.GetReflectionProbeDebugMipCount();
        reflection_probe_mip_slider.SetRange(0.0f, float(mip_count > 1 ? mip_count - 1 : 1));
        reflection_probe_mip_slider.SetValue(int(render_path.GetReflectionProbeDebugMip()));
        reflection_probe_mip_slider.SetEnabled(mip_count > 1);
        diagnostic_pick_center_button.SetEnabled(diagnostic_controls_enabled);
        diagnostic_resolution_combo.SetEnabled(diagnostic_controls_enabled && !diagnostic_active);
        diagnostic_samples_combo.SetEnabled(diagnostic_controls_enabled && !diagnostic_active);
        diagnostic_bounces_combo.SetEnabled(diagnostic_controls_enabled && !diagnostic_active);
        diagnostic_regenerate_atlas_checkbox.SetEnabled(diagnostic_controls_enabled && !diagnostic_active);
        diagnostic_pause_before_save_checkbox.SetEnabled(diagnostic_controls_enabled && !diagnostic_active);
        diagnostic_bake_button.SetEnabled(
            diagnostic_controls_enabled && render_path.HasLightmapDiagnosticObject());
        diagnostic_resume_button.SetEnabled(render_path.IsLightmapDiagnosticPaused());
        diagnostic_discard_button.SetEnabled(diagnostic_active);
    }
    render_path.SetInputActive(is_window_active);
    wi::Application::Update(dt);
}

std::string NewPipelineClientApp::GetWindowTitle() const
{
    return std::string{"NewPipeline_Wicked Client ["} + ToString(runtime_config.remote_source) +
        "] [LM-DIAG-R1]";
}
} // namespace wicked_newpipeline
