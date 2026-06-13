// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// PCSX2x6 libretro core frontend.
//
// Threading model:
//  - retro_load_game() spawns a dedicated CPU thread which runs the usual
//    VMManager::Execute() loop.
//  - Host::PumpMessagesOnCPUThread() is invoked by the core once per emulated
//    frame (at CPU vsync). We use it as the pacing point: the CPU thread grabs
//    the presented frame into a buffer, signals retro_run(), then blocks until
//    the frontend asks for the next frame.
//  - retro_run() hands one "run token" to the CPU thread, waits (with timeout,
//    so slow boots don't freeze the frontend) for the frame, and uploads it.
//
// Framebuffer: software renderer + GSSetFramebufferReadback() readback.
// Audio: SPU2::CustomOutputStreamFactory injects a LibretroAudioStream.

#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include <cstdio>
#include <cstdlib>


#include "common/Assertions.h"
#include "common/Console.h"
#include "common/CrashHandler.h"
#include "common/Error.h"
#include "common/HostSys.h"
#include "common/FileSystem.h"
#include "common/MemorySettingsInterface.h"
#include "common/Path.h"
#include "common/ProgressCallback.h"
#include "common/SettingsWrapper.h"
#include "common/StringUtil.h"
#include "common/WindowInfo.h"

#include "pcsx2/Achievements.h"
#include "pcsx2/Config.h"
#include "pcsx2/GS/GS.h"
#include "pcsx2/Host/AudioStream.h"
#include "pcsx2/Host/AudioStreamTypes.h"
#include "pcsx2/SIO/Pad/PadDualshock2.h"
#include "pcsx2/SPU2/spu2.h"
#include "pcsx2/Host.h"
#include "pcsx2/INISettingsInterface.h"
#include "pcsx2/ImGui/FullscreenUI.h"
#include "pcsx2/ImGui/ImGuiFullscreen.h"
#include "pcsx2/ImGui/ImGuiManager.h"
#include "pcsx2/Input/InputManager.h"
#include "pcsx2/DEV9/ACJV.h"
#include "pcsx2/MTGS.h"
#include "pcsx2/MemoryTypes.h"
#include "pcsx2/PerformanceMetrics.h"
#include "pcsx2/SIO/Pad/Pad.h"
#include "pcsx2/SaveState.h"
#include "pcsx2/USB/USB.h"
#include "pcsx2/VMManager.h"
#include "pcsx2/ps2/BiosTools.h"

#include "svnrev.h"

#include "libretro.h"

#include "fmt/format.h"


namespace LibretroHost
{
	// libretro callbacks
	static retro_environment_t s_environ_cb;
	static retro_video_refresh_t s_video_cb;
	static retro_audio_sample_batch_t s_audio_batch_cb;
	static retro_input_poll_t s_input_poll_cb;
	static retro_input_state_t s_input_state_cb;
	static retro_log_printf_t s_log_cb;

	// configuration
	static MemorySettingsInterface s_settings_interface;
	static std::string s_system_dir;

	// CPU thread management. The thread persists across game sessions because
	// VMManager::Internal::CPUThreadInitialize may only run once per process.
	static std::thread s_cpu_thread;
	static std::atomic_bool s_running{false};
	static VMBootParameters s_boot_params;
	static std::mutex s_session_mutex;
	static std::condition_variable s_session_cv;
	static bool s_boot_requested = false;
	static bool s_exit_requested = false;
	static bool s_session_active = false;

	// frame pacing: retro_run() posts a run token, CPU thread posts frame-done
	static std::mutex s_frame_mutex;
	static std::condition_variable s_frame_cv;
	static bool s_run_token = false;
	static bool s_frame_ready = false;

	// frame buffer handed from CPU thread to retro_run (guarded by s_frame_mutex)
	static std::vector<u32> s_frame_pixels;
	static u32 s_frame_width = 0;
	static u32 s_frame_height = 0;

	// deferred work queue for Host::RunOnCPUThread
	static std::mutex s_cpu_work_mutex;
	static std::deque<std::function<void()>> s_cpu_work;
	static std::atomic_bool s_cpu_work_pending{false};
	static std::thread::id s_cpu_thread_id;

	static constexpr u32 DEFAULT_WIDTH = 640;
	static constexpr u32 DEFAULT_HEIGHT = 480;
	static constexpr u32 MAX_UPSCALE = 4;
	static constexpr u32 MAX_WIDTH = DEFAULT_WIDTH * MAX_UPSCALE;
	static constexpr u32 MAX_HEIGHT = DEFAULT_HEIGHT * MAX_UPSCALE;

	// current output (readback) resolution; follows the upscale option
	static std::atomic<u32> s_out_width{DEFAULT_WIDTH};
	static std::atomic<u32> s_out_height{DEFAULT_HEIGHT};

	// core option state
	static std::vector<std::string> s_bios_names;
	static u32 s_opt_upscale = 1;


	// libretro port -> PCSX2 pad index
	static std::vector<u32> s_pad_map = {0, 1};

	// GunCon2 lightguns on USB ports (bit 0 = USB1, bit 1 = USB2)
	static u32 s_lightgun_mask = 0;

	// rumble state: packed as (large << 16) | small, each 0..65535
	static std::array<std::atomic<u32>, 8> s_pad_rumble;
	static bool s_rumble_enabled = true;
	static retro_rumble_interface s_rumble_interface = {};

	static constexpr u32 SAMPLE_RATE = 48000;
	static constexpr u32 MAX_AUDIO_FRAMES_PER_RUN = 2048;

	static std::atomic<u32> s_vm_fps_bits{0};
	static std::atomic<u32> s_audio_sample_rate{SAMPLE_RATE};
	static bool s_memory_map_sent = false;
	static std::atomic<u32> s_aspect_bits{0};

	class LibretroAudioStream final : public AudioStream
	{
	public:
		LibretroAudioStream(u32 sample_rate, const AudioStreamParameters& parameters)
			: AudioStream(sample_rate, parameters)
		{
		}

		void Initialize()
		{
			BaseInitialize(&StereoSampleReaderImpl, false);
		}

		u32 PullFrames(SampleType* dest, u32 max_frames)
		{
			const u32 frames = std::min(GetBufferedFramesRelaxed(), max_frames);
			if (frames > 0)
				ReadFrames(dest, frames);
			return frames;
		}
	};

	static LibretroAudioStream* s_audio_stream = nullptr;
	static std::atomic<u64> s_audio_frames_output{0};

	static void FramebufferReadbackCallback(const u32* pixels, u32 pitch_px, u32 width, u32 height)
	{
		std::unique_lock lock(s_frame_mutex);
		s_frame_pixels.resize(static_cast<size_t>(width) * height);
		for (u32 y = 0; y < height; y++)
		{
			const u32* src = pixels + static_cast<size_t>(y) * pitch_px;
			u32* dst = s_frame_pixels.data() + static_cast<size_t>(y) * width;
			for (u32 x = 0; x < width; x++)
			{
				const u32 px = src[x];
				// RGBA -> XRGB8888
				dst[x] = (px & 0xFF00FF00u) | ((px & 0xFFu) << 16) | ((px >> 16) & 0xFFu);
			}
		}
		s_frame_width = width;
		s_frame_height = height;
	}

	static std::unique_ptr<AudioStream> CreateLibretroAudioStream(u32 sample_rate, const AudioStreamParameters& parameters)
	{
		std::unique_ptr<LibretroAudioStream> stream = std::make_unique<LibretroAudioStream>(sample_rate, parameters);
		stream->Initialize();
		s_audio_stream = stream.get();
		s_audio_sample_rate.store(sample_rate, std::memory_order_release);
		return stream;
	}

	static bool InitializeConfig();
	static void SettingsOverride();
	static void CPUThreadMain();
	static void DrainCPUWork();
	static void RegisterCoreOptions();
	static void ReadCoreOptions(bool startup);

	static void HostLogCallback(LOGLEVEL level, ConsoleColors color, std::string_view message)
	{
		if (!s_log_cb)
		{
			std::fprintf(stderr, "%.*s\n", static_cast<int>(message.size()), message.data());
			return;
		}

		retro_log_level rl = RETRO_LOG_INFO;
		if (level == LOGLEVEL_ERROR)
			rl = RETRO_LOG_ERROR;
		else if (level == LOGLEVEL_WARNING)
			rl = RETRO_LOG_WARN;
		else if (level >= LOGLEVEL_DEV)
			rl = RETRO_LOG_DEBUG;
		s_log_cb(rl, "[PCSX2x6] %.*s\n", static_cast<int>(message.size()), message.data());
	}
} // namespace LibretroHost

using namespace LibretroHost;

static void ShutdownCoreAtExit()
{
	if (s_cpu_thread.joinable())
	{
		{
			std::unique_lock lock(s_session_mutex);
			s_exit_requested = true;
		}
		s_session_cv.notify_all();
		s_cpu_thread.join();
	}
}

//////////////////////////////////////////////////////////////////////////
// Config / boot
//////////////////////////////////////////////////////////////////////////

bool LibretroHost::InitializeConfig()
{
	const char* system_dir = nullptr;
	if (s_environ_cb && s_environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system_dir) && system_dir)
		s_system_dir = Path::Combine(system_dir, "pcsx2");
	else
		s_system_dir = "pcsx2";

	EmuFolders::AppRoot = s_system_dir;
	EmuFolders::DataRoot = s_system_dir;
	EmuFolders::Resources = Path::Combine(s_system_dir, "resources");
	EmuFolders::UserResources = EmuFolders::Resources;
	EmuFolders::Settings = Path::Combine(s_system_dir, "inis");

	CrashHandler::SetWriteDirectory(EmuFolders::DataRoot);

	if (!FileSystem::DirectoryExists(EmuFolders::Resources.c_str()))
	{
		Console.ErrorFmt("PCSX2x6 resources directory not found at '{}'. Copy the 'resources' directory there.",
			EmuFolders::Resources);
		return false;
	}

	const char* error;
	if (!VMManager::PerformEarlyHardwareChecks(&error))
	{
		Console.ErrorFmt("Hardware check failed: {}", error);
		return false;
	}

	{
		const std::string roboto_path =
			EmuFolders::GetOverridableResourcePath("fonts" FS_OSPATH_SEPARATOR_STR "Roboto-Regular.ttf");
		const auto roboto_data = FileSystem::MapBinaryFileForRead(roboto_path.c_str());
		if (roboto_data.empty())
		{
			Console.ErrorFmt("Failed to load font file '{}'.", roboto_path);
			return false;
		}

		std::vector<ImGuiManager::FontInfo> fonts;
		ImGuiManager::FontInfo fi{};
		fi.data = roboto_data;
		fi.exclude_ranges = {};
		fi.face_name = nullptr;
		fi.is_emoji_font = false;
		fonts.push_back(fi);
		ImGuiManager::SetFonts(std::move(fonts));
	}

	MemorySettingsInterface& si = s_settings_interface;

	static bool s_settings_layer_registered = false;
	if (!s_settings_layer_registered)
	{
		Host::Internal::SetBaseSettingsLayer(&si);
		s_settings_layer_registered = true;
	}

	VMManager::SetDefaultSettings(si, true, true, true, true, true);

	{
		const std::string ini_path = Path::Combine(EmuFolders::Settings, "PCSX2.ini");
		INISettingsInterface ini(ini_path);
		if (FileSystem::FileExists(ini_path.c_str()) && ini.Load())
		{
			static constexpr const char* merge_sections[] = {
				"EmuCore",
				"EmuCore/Speedhacks",
				"EmuCore/CPU",
				"EmuCore/CPU/Recompiler",
				"EmuCore/GS",
				"EmuCore/Gamefixes",
				"MemoryCards",
				"DEV9/Eth",
				"DEV9/Hdd",
			};

			u32 merged = 0;
			for (const char* section : merge_sections)
			{
				for (const auto& [key, value] : ini.GetKeyValueList(section))
				{
					s_settings_interface.SetStringValue(section, key.c_str(), value.c_str());
					merged++;
				}
			}
			Console.WriteLnFmt("Adopted {} settings from standalone config '{}'.", merged, ini_path);
		}
	}

	VMManager::Internal::LoadStartupSettings();

	EmuFolders::EnsureFoldersExist();
	return true;
}

void LibretroHost::SettingsOverride()
{
	s_settings_interface.SetBoolValue("EmuCore/GS", "FrameLimitEnable", false);
	s_settings_interface.SetIntValue("EmuCore/GS", "VsyncEnable", false);

	s_settings_interface.SetBoolValue("InputSources", "SDL", false);
	s_settings_interface.SetBoolValue("InputSources", "XInput", false);
	s_settings_interface.ClearSection("Hotkeys");

	s_settings_interface.SetBoolValue("Logging", "EnableSystemConsole", false);

	s_settings_interface.SetIntValue("EmuCore", "SavestateCompressionType",
		static_cast<int>(SavestateCompressionMethod::Uncompressed));

	if (s_settings_interface.GetStringValue("Filenames", "BIOS").empty())
	{
		FileSystem::FindResultsArray files;
		FileSystem::FindFiles(EmuFolders::Bios.c_str(), "*", FILESYSTEM_FIND_FILES, &files);
		for (const FILESYSTEM_FIND_DATA& fd : files)
		{
			u32 version, region;
			std::string description, zone;
			if (IsBIOS(fd.FileName.c_str(), version, description, region, zone))
			{
				const std::string filename(Path::GetFileName(fd.FileName));
				Console.WriteLnFmt("Auto-selected BIOS: {} ({})", filename, description);
				s_settings_interface.SetStringValue("Filenames", "BIOS", filename.c_str());
				break;
			}
		}
	}
}

void LibretroHost::RegisterCoreOptions()
{
	s_bios_names.clear();
	s_bios_names.push_back("auto");
	{
		const char* system_dir = nullptr;
		if (s_environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system_dir) && system_dir)
		{
			FileSystem::FindResultsArray files;
			FileSystem::FindFiles(Path::Combine(Path::Combine(system_dir, "pcsx2"), "bios").c_str(), "*",
				FILESYSTEM_FIND_FILES, &files);
			for (const FILESYSTEM_FIND_DATA& fd : files)
			{
				u32 version, region;
				std::string description, zone;
				if (IsBIOS(fd.FileName.c_str(), version, description, region, zone))
					s_bios_names.push_back(std::string(Path::GetFileName(fd.FileName)));
			}
		}
	}

	static retro_core_option_v2_category categories[] = {
		{"system", "System", "BIOS and boot behaviour."},
		{"graphics", "Graphics", "Renderer, resolution and image quality."},
		{"patches", "Patches", "Built-in game patches (widescreen, no-interlacing)."},
		{"performance", "Performance", "Speed hacks. May break games."},
		{nullptr, nullptr, nullptr},
	};

	retro_core_option_v2_definition definitions[] = {
		{"pcsx2_bios", "BIOS", nullptr, "BIOS image to use, from <system>/pcsx2/bios. Requires restart.", nullptr,
			"system", {{nullptr, nullptr}}, "auto"},
		{"pcsx2_fast_boot", "Fast Boot", nullptr, "Skip the BIOS boot animation. Requires restart.", nullptr,
			"system", {{"enabled", nullptr}, {"disabled", nullptr}, {nullptr, nullptr}}, "enabled"},
		{"pcsx2_renderer", "Renderer", nullptr,
			"Hardware renderer API, or the software renderer. Applies on the fly.",
			nullptr, "graphics",
			{{"software", "Software"}, {"vulkan", "Vulkan (Hardware)"}, {"opengl", "OpenGL (Hardware)"},
				{nullptr, nullptr}},
			"opengl"},
		{"pcsx2_upscale_multiplier", "Internal Resolution", nullptr,
			"Internal rendering resolution multiplier. Also scales the output framebuffer.",
			nullptr, "graphics",
			{{"1", "1x Native (640x480)"}, {"2", "2x Native (1280x960)"}, {"3", "3x Native (1920x1440)"},
				{"4", "4x Native (2560x1920)"}, {nullptr, nullptr}},
			"1"},
		{"pcsx2_blending_accuracy", "Blending Accuracy", nullptr,
			"Higher levels emulate more PS2 blending effects correctly at a GPU cost.", nullptr, "graphics",
			{{"minimum", "Minimum"}, {"basic", "Basic (Recommended)"}, {"medium", "Medium"}, {"high", "High"},
				{"full", "Full (Slow)"}, {"maximum", "Maximum (Very Slow)"}, {nullptr, nullptr}},
			"basic"},
		{"pcsx2_texture_filtering", "Texture Filtering", nullptr,
			"Bilinear (PS2) replicates the console.", nullptr, "graphics",
			{{"nearest", "Nearest"}, {"bilinear_ps2", "Bilinear (PS2)"}, {"bilinear_forced", "Bilinear (Forced)"},
				{"bilinear_forced_sprite", "Bilinear (Forced excluding sprites)"}, {nullptr, nullptr}},
			"bilinear_ps2"},
		{"pcsx2_trilinear_filtering", "Trilinear Filtering", nullptr, nullptr, nullptr, "graphics",
			{{"auto", "Automatic (Default)"}, {"off", "Off"}, {"ps2", "Trilinear (PS2)"}, {"forced", "Trilinear (Forced)"},
				{nullptr, nullptr}},
			"auto"},
		{"pcsx2_anisotropic_filtering", "Anisotropic Filtering", nullptr,
			"Reduces texture aliasing at steep angles.", nullptr, "graphics",
			{{"0", "Off"}, {"2", "2x"}, {"4", "4x"}, {"8", "8x"}, {"16", "16x"}, {nullptr, nullptr}}, "0"},
		{"pcsx2_dithering", "Dithering", nullptr,
			"Unscaled (default) replicates PS2 dithering.", nullptr, "graphics",
			{{"0", "Off"}, {"1", "Scaled"}, {"2", "Unscaled (Default)"}, {nullptr, nullptr}}, "2"},
		{"pcsx2_mipmapping", "Hardware Mipmapping", nullptr, nullptr, nullptr, "graphics",
			{{"enabled", nullptr}, {"disabled", nullptr}, {nullptr, nullptr}}, "enabled"},
		{"pcsx2_deinterlace_mode", "Deinterlacing", nullptr,
			"Automatic uses the GameDB-recommended mode per game.", nullptr, "graphics",
			{{"0", "Automatic (Default)"}, {"1", "Off"}, {"2", "Weave (TFF)"}, {"3", "Weave (BFF)"},
				{"4", "Bob (TFF)"}, {"5", "Bob (BFF)"}, {"6", "Blend (TFF)"}, {"7", "Blend (BFF)"},
				{nullptr, nullptr}},
			"0"},
		{"pcsx2_fxaa", "FXAA", nullptr, "Cheap post-process anti-aliasing.", nullptr, "graphics",
			{{"disabled", nullptr}, {"enabled", nullptr}, {nullptr, nullptr}}, "disabled"},
		{"pcsx2_reduce_shader_precision", "Reduce Shader Precision", nullptr,
			"Use mediump float in the fragment shader for speed on weak GPUs (e.g. RPi5/V3D). "
			"Helps GPU-bound scenes but causes minor texture shimmer. Hardware (OpenGL) renderer only.",
			nullptr, "performance",
			{{"disabled", "Disabled (Accurate)"}, {"enabled", "Enabled (Faster)"}, {nullptr, nullptr}}, "disabled"},
		{"pcsx2_aspect_ratio", "Aspect Ratio", nullptr,
			"Automatic reports 16:9 when widescreen patches are enabled, 4:3 otherwise.", nullptr, "graphics",
			{{"auto", "Automatic"}, {"4:3", nullptr}, {"16:9", nullptr}, {nullptr, nullptr}}, "auto"},
		{"pcsx2_widescreen_patches", "Widescreen Patches", nullptr,
			"Enable built-in 16:9 widescreen patches where available.", nullptr, "patches",
			{{"disabled", nullptr}, {"enabled", nullptr}, {nullptr, nullptr}}, "disabled"},
		{"pcsx2_no_interlacing_patches", "No-Interlacing Patches", nullptr,
			"Enable built-in progressive-output patches where available.", nullptr, "patches",
			{{"disabled", nullptr}, {"enabled", nullptr}, {nullptr, nullptr}}, "disabled"},
		{"pcsx2_ee_cycle_rate", "EE Cycle Rate", nullptr,
			"Underclock or overclock the Emotion Engine. Default 100%. May break games.", nullptr, "performance",
			{{"-3", "50% (Underclock)"}, {"-2", "60% (Underclock)"}, {"-1", "75% (Underclock)"},
				{"0", "100% (Default)"}, {"1", "130% (Overclock)"}, {"2", "180% (Overclock)"},
				{"3", "300% (Overclock)"}, {nullptr, nullptr}},
			"0"},
		{"pcsx2_ee_cycle_skip", "EE Cycle Skip", nullptr,
			"Makes the EE skip cycles. Helps some games, breaks others.", nullptr, "performance",
			{{"0", "Disabled (Default)"}, {"1", "Mild"}, {"2", "Moderate"}, {"3", "Maximum"}, {nullptr, nullptr}},
			"0"},
		{nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, {{nullptr, nullptr}}, nullptr},
	};

	for (retro_core_option_v2_definition& def : definitions)
	{
		if (!def.key || std::strcmp(def.key, "pcsx2_bios") != 0)
			continue;
		const size_t max_bios = std::min(s_bios_names.size(), std::size(def.values) - 1);
		for (size_t i = 0; i < max_bios; i++)
			def.values[i] = {s_bios_names[i].c_str(), nullptr};
		def.values[max_bios] = {nullptr, nullptr};
		break;
	}

	unsigned version = 0;
	if (s_environ_cb(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &version) && version >= 2)
	{
		retro_core_options_v2 options = {categories, definitions};
		s_environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, &options);
		return;
	}

	static std::vector<std::string> legacy_storage;
	legacy_storage.clear();
	std::vector<retro_variable> legacy;
	for (const retro_core_option_v2_definition& def : definitions)
	{
		if (!def.key)
			break;
		std::string str = fmt::format("{}; ", def.desc);
		str += def.default_value;
		for (const retro_core_option_value& v : def.values)
		{
			if (!v.value)
				break;
			if (std::strcmp(v.value, def.default_value) != 0)
				str += fmt::format("|{}", v.value);
		}
		legacy_storage.push_back(std::move(str));
		legacy.push_back({def.key, legacy_storage.back().c_str()});
	}
	legacy.push_back({nullptr, nullptr});
	s_environ_cb(RETRO_ENVIRONMENT_SET_VARIABLES, legacy.data());
}

void LibretroHost::ReadCoreOptions(bool startup)
{
	const auto get_option = [](const char* key, const char* fallback) -> const char* {
		retro_variable var = {key, nullptr};
		if (s_environ_cb && s_environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
			return var.value;
		return fallback;
	};

	const char* renderer = get_option("pcsx2_renderer", "software");
	GSRendererType renderer_type = GSRendererType::SW;
	if (std::strcmp(renderer, "vulkan") == 0)
		renderer_type = GSRendererType::VK;
	else if (std::strcmp(renderer, "opengl") == 0)
		renderer_type = GSRendererType::OGL;
	s_settings_interface.SetIntValue("EmuCore/GS", "Renderer", static_cast<int>(renderer_type));

	const u32 upscale = std::clamp<u32>(StringUtil::FromChars<u32>(get_option("pcsx2_upscale_multiplier", "1")).value_or(1), 1, MAX_UPSCALE);
	s_settings_interface.SetFloatValue("EmuCore/GS", "upscale_multiplier", static_cast<float>(upscale));
	s_opt_upscale = upscale;
	s_out_width.store(DEFAULT_WIDTH * upscale, std::memory_order_release);
	s_out_height.store(DEFAULT_HEIGHT * upscale, std::memory_order_release);
	GSSetFramebufferReadback(&FramebufferReadbackCallback, DEFAULT_WIDTH * upscale, DEFAULT_HEIGHT * upscale);

	const auto get_int_option = [&get_option](const char* key, const char* fallback) {
		return StringUtil::FromChars<int>(get_option(key, fallback)).value_or(StringUtil::FromChars<int>(fallback).value_or(0));
	};

	static constexpr std::pair<const char*, AccBlendLevel> blend_levels[] = {
		{"minimum", AccBlendLevel::Minimum}, {"basic", AccBlendLevel::Basic}, {"medium", AccBlendLevel::Medium},
		{"high", AccBlendLevel::High}, {"full", AccBlendLevel::Full}, {"maximum", AccBlendLevel::Maximum}};
	const char* blend = get_option("pcsx2_blending_accuracy", "basic");
	for (const auto& [name, level] : blend_levels)
	{
		if (std::strcmp(blend, name) == 0)
		{
			s_settings_interface.SetIntValue("EmuCore/GS", "accurate_blending_unit", static_cast<int>(level));
			break;
		}
	}

	static constexpr std::pair<const char*, BiFiltering> bi_filters[] = {
		{"nearest", BiFiltering::Nearest}, {"bilinear_ps2", BiFiltering::PS2},
		{"bilinear_forced", BiFiltering::Forced}, {"bilinear_forced_sprite", BiFiltering::Forced_But_Sprite}};
	const char* bi = get_option("pcsx2_texture_filtering", "bilinear_ps2");
	for (const auto& [name, mode] : bi_filters)
	{
		if (std::strcmp(bi, name) == 0)
		{
			s_settings_interface.SetIntValue("EmuCore/GS", "filter", static_cast<int>(mode));
			break;
		}
	}

	static constexpr std::pair<const char*, TriFiltering> tri_filters[] = {
		{"auto", TriFiltering::Automatic}, {"off", TriFiltering::Off}, {"ps2", TriFiltering::PS2},
		{"forced", TriFiltering::Forced}};
	const char* tri = get_option("pcsx2_trilinear_filtering", "auto");
	for (const auto& [name, mode] : tri_filters)
	{
		if (std::strcmp(tri, name) == 0)
		{
			s_settings_interface.SetIntValue("EmuCore/GS", "TriFilter", static_cast<int>(mode));
			break;
		}
	}

	s_settings_interface.SetIntValue("EmuCore/GS", "MaxAnisotropy", get_int_option("pcsx2_anisotropic_filtering", "0"));
	s_settings_interface.SetIntValue("EmuCore/GS", "dithering_ps2", get_int_option("pcsx2_dithering", "2"));
	s_settings_interface.SetBoolValue("EmuCore/GS", "hw_mipmap",
		std::strcmp(get_option("pcsx2_mipmapping", "enabled"), "enabled") == 0);
	s_settings_interface.SetIntValue("EmuCore/GS", "deinterlace_mode", get_int_option("pcsx2_deinterlace_mode", "0"));
	s_settings_interface.SetBoolValue("EmuCore/GS", "fxaa",
		std::strcmp(get_option("pcsx2_fxaa", "disabled"), "enabled") == 0);
	s_settings_interface.SetBoolValue("EmuCore/GS", "GLESReducedPrecision",
		std::strcmp(get_option("pcsx2_reduce_shader_precision", "disabled"), "enabled") == 0);

	const bool widescreen = std::strcmp(get_option("pcsx2_widescreen_patches", "disabled"), "enabled") == 0;
	s_settings_interface.SetBoolValue("EmuCore", "EnableWideScreenPatches", widescreen);
	s_settings_interface.SetBoolValue("EmuCore", "EnableNoInterlacingPatches",
		std::strcmp(get_option("pcsx2_no_interlacing_patches", "disabled"), "enabled") == 0);

	const char* aspect = get_option("pcsx2_aspect_ratio", "auto");
	float aspect_value = 4.0f / 3.0f;
	if (std::strcmp(aspect, "16:9") == 0 || (std::strcmp(aspect, "auto") == 0 && widescreen))
		aspect_value = 16.0f / 9.0f;
	s_aspect_bits.store(std::bit_cast<u32>(aspect_value), std::memory_order_release);

	s_settings_interface.SetIntValue("EmuCore/Speedhacks", "EECycleRate", get_int_option("pcsx2_ee_cycle_rate", "0"));
	s_settings_interface.SetIntValue("EmuCore/Speedhacks", "EECycleSkip", get_int_option("pcsx2_ee_cycle_skip", "0"));

	{
		// System 246/256 arcade: input is JVS (2 players max), so no multitap and no
		// DualShock2 stick tuning. Two pads are still configured for the brief window
		// before the game starts its JVS driver (and so the emulated ports exist).
		s_settings_interface.SetBoolValue("Pad", "MultitapPort1", false);
		s_settings_interface.SetBoolValue("Pad", "MultitapPort2", false);

		s_pad_map.clear();
		s_pad_map.push_back(0); // P1
		s_pad_map.push_back(1); // P2

		for (const u32 pad : s_pad_map)
		{
			const std::string section = fmt::format("Pad{}", pad + 1);
			s_settings_interface.SetStringValue(section.c_str(), "Type", "DualShock2");
			s_settings_interface.SetFloatValue(section.c_str(), "AxisScale", 1.33f);
			s_settings_interface.SetFloatValue(section.c_str(), "Deadzone", 0.0f);
		}

		// JVS arcade controls have no vibration.
		s_rumble_enabled = false;
	}

	if (startup)
	{
		const char* bios = get_option("pcsx2_bios", "auto");
		if (std::strcmp(bios, "auto") != 0)
			s_settings_interface.SetStringValue("Filenames", "BIOS", bios);

		s_settings_interface.SetBoolValue("EmuCore", "EnableFastBoot",
			std::strcmp(get_option("pcsx2_fast_boot", "enabled"), "enabled") == 0);
	}
}

void LibretroHost::CPUThreadMain()
{
	s_cpu_thread_id = std::this_thread::get_id();

	const bool init_ok = VMManager::Internal::CPUThreadInitialize();
	if (!init_ok)
		Console.Error("CPUThreadInitialize() failed.");

	for (;;)
	{
		{
			std::unique_lock lock(s_session_mutex);
			s_session_cv.wait(lock, []() { return s_boot_requested || s_exit_requested; });
			if (s_exit_requested)
				break;
			s_boot_requested = false;
		}

		if (init_ok)
		{
			VMManager::ApplySettings();

			Error vm_error;
			if (VMManager::Initialize(s_boot_params, &vm_error) == VMBootResult::StartupSuccess)
			{
				VMManager::SetState(VMState::Running);
				VMManager::SetLimiterMode(LimiterModeType::Unlimited);
				while (VMManager::GetState() == VMState::Running && s_running.load(std::memory_order_acquire))
					VMManager::Execute();
				VMManager::Shutdown(false);
			}
			else
			{
				Console.Error("VMManager::Initialize() failed: {}", vm_error.GetDescription());
			}
		}

		s_running.store(false, std::memory_order_release);

		{
			std::unique_lock lock(s_frame_mutex);
			s_frame_ready = true;
		}
		s_frame_cv.notify_all();

		{
			std::unique_lock lock(s_session_mutex);
			s_session_active = false;
		}
		s_session_cv.notify_all();
	}

	if (init_ok)
		VMManager::Internal::CPUThreadShutdown();
}

void LibretroHost::DrainCPUWork()
{
	std::deque<std::function<void()>> work;
	{
		std::unique_lock lock(s_cpu_work_mutex);
		work.swap(s_cpu_work);
		s_cpu_work_pending.store(false, std::memory_order_release);
	}
	for (auto& fn : work)
		fn();
}

//////////////////////////////////////////////////////////////////////////
// libretro entry points
//////////////////////////////////////////////////////////////////////////

void retro_set_environment(retro_environment_t cb)
{
	s_environ_cb = cb;

	bool no_game = false;
	cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_game);

	retro_log_callback log_cb{};
	if (cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log_cb))
		s_log_cb = log_cb.log;

	RegisterCoreOptions();
}

void retro_set_video_refresh(retro_video_refresh_t cb) { s_video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) {}
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { s_audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { s_input_poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { s_input_state_cb = cb; }

unsigned retro_api_version(void)
{
	return RETRO_API_VERSION;
}

void retro_get_system_info(struct retro_system_info* info)
{
	std::memset(info, 0, sizeof(*info));
	info->library_name = "PCSX2x6";
	info->library_version = GIT_REV;
	info->valid_extensions = "iso|chd|cso|zso|gz|bin|mdf|nrg|elf|irx|acgame";
	info->need_fullpath = true;
	info->block_extract = true;
}

void retro_get_system_av_info(struct retro_system_av_info* info)
{
	const u32 fps_bits = s_vm_fps_bits.load(std::memory_order_acquire);
	const float fps = fps_bits ? std::bit_cast<float>(fps_bits) : 59.94f;

	std::memset(info, 0, sizeof(*info));
	info->geometry.base_width = s_out_width.load(std::memory_order_acquire);
	info->geometry.base_height = s_out_height.load(std::memory_order_acquire);
	info->geometry.max_width = MAX_WIDTH;
	info->geometry.max_height = MAX_HEIGHT;
	const u32 aspect_bits = s_aspect_bits.load(std::memory_order_acquire);
	info->geometry.aspect_ratio = aspect_bits ? std::bit_cast<float>(aspect_bits) : (4.0f / 3.0f);
	info->timing.fps = static_cast<double>(fps);
	info->timing.sample_rate = static_cast<double>(s_audio_sample_rate.load(std::memory_order_acquire));
}

void retro_init(void)
{
	static bool s_crash_handler_installed = false;
	if (!s_crash_handler_installed)
	{
		CrashHandler::Install();
		std::atexit(&ShutdownCoreAtExit);
		s_crash_handler_installed = true;
	}

	Log::SetHostOutputLevel(LOGLEVEL_INFO, &HostLogCallback);
}

void retro_deinit(void)
{
	if (s_cpu_thread.joinable())
	{
		{
			std::unique_lock lock(s_session_mutex);
			s_exit_requested = true;
		}
		s_session_cv.notify_all();
		s_cpu_thread.join();
		{
			std::unique_lock lock(s_session_mutex);
			s_exit_requested = false;
		}
	}
}

bool retro_load_game(const struct retro_game_info* game)
{
	if (!game || !game->path)
		return false;

	enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
	if (!s_environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
	{
		Console.Error("XRGB8888 pixel format not supported by frontend.");
		return false;
	}

	if (!InitializeConfig())
		return false;

	ReadCoreOptions(true);
	SettingsOverride();

	SPU2::CustomOutputStreamFactory = &CreateLibretroAudioStream;
	s_environ_cb(RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE, &s_rumble_interface);

	// Arcade manifest: show the JVS roles in the frontend instead of PS2 pad names.
	// (The per-game button numbering depends on the fighting layout, so the face
	// buttons keep their pad names; the system buttons are what matters here.)
	if (StringUtil::EndsWithNoCase(game->path, ".acgame"))
	{
		static const struct retro_input_descriptor arcade_descs[] = {
			{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "Up"},
			{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "Down"},
			{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "Left"},
			{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "Right"},
			{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y, "Button 1 (Square)"},
			{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X, "Button 2 (Triangle)"},
			{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "Button (Cross)"},
			{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "Button (Circle)"},
			{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L, "Button (6-button games)"},
			{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R, "Button (6-button games)"},
			{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start"},
			{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Insert Coin"},
			{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3, "Test Menu"},
			{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3, "Service"},
			{1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "Up"},
			{1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "Down"},
			{1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "Left"},
			{1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "Right"},
			{1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y, "Button 1 (Square)"},
			{1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X, "Button 2 (Triangle)"},
			{1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "Button (Cross)"},
			{1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "Button (Circle)"},
			{1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L, "Button (6-button games)"},
			{1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R, "Button (6-button games)"},
			{1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start"},
			{1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Insert Coin"},
			{1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3, "Test Menu"},
			{1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3, "Service"},
			{0, 0, 0, 0, nullptr},
		};
		s_environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, const_cast<retro_input_descriptor*>(arcade_descs));
	}

	s_boot_params = VMBootParameters();
	s_boot_params.filename = game->path;

	{
		std::unique_lock lock(s_frame_mutex);
		s_run_token = false;
		s_frame_ready = false;
		s_frame_width = 0;
		s_frame_height = 0;
	}

	s_running.store(true, std::memory_order_release);
	{
		std::unique_lock lock(s_session_mutex);
		s_boot_requested = true;
		s_session_active = true;
	}
	if (!s_cpu_thread.joinable())
		s_cpu_thread = std::thread(CPUThreadMain);
	s_session_cv.notify_all();
	return true;
}

bool retro_load_game_special(unsigned game_type, const struct retro_game_info* info, size_t num_info)
{
	return false;
}

void retro_unload_game(void)
{
	if (!s_cpu_thread.joinable())
		return;

	s_running.store(false, std::memory_order_release);
	if (VMManager::HasValidVM())
		VMManager::SetState(VMState::Stopping);

	{
		std::unique_lock lock(s_frame_mutex);
		s_run_token = true;
		s_frame_cv.notify_all();
	}

	{
		std::unique_lock lock(s_session_mutex);
		s_session_cv.wait(lock, []() { return !s_session_active; });
	}

	s_audio_stream = nullptr;
	GSSetFramebufferReadback(nullptr, 0, 0);
	s_memory_map_sent = false;
}

void retro_reset(void)
{
	if (s_running.load(std::memory_order_acquire))
		Host::RunOnCPUThread([]() { VMManager::Reset(); });
}

// Translate the generic (DS4-ish) binding hints from the ACJV layout tables to
// retropad button ids. PS2 face buttons follow the usual libretro convention
// (Cross=B, Circle=A, Square=Y, Triangle=X).
static int GenericBindingToRetroButton(GenericInputBinding binding)
{
	switch (binding)
	{
		case GenericInputBinding::DPadUp: return RETRO_DEVICE_ID_JOYPAD_UP;
		case GenericInputBinding::DPadDown: return RETRO_DEVICE_ID_JOYPAD_DOWN;
		case GenericInputBinding::DPadLeft: return RETRO_DEVICE_ID_JOYPAD_LEFT;
		case GenericInputBinding::DPadRight: return RETRO_DEVICE_ID_JOYPAD_RIGHT;
		case GenericInputBinding::Cross: return RETRO_DEVICE_ID_JOYPAD_B;
		case GenericInputBinding::Circle: return RETRO_DEVICE_ID_JOYPAD_A;
		case GenericInputBinding::Square: return RETRO_DEVICE_ID_JOYPAD_Y;
		case GenericInputBinding::Triangle: return RETRO_DEVICE_ID_JOYPAD_X;
		case GenericInputBinding::L1: return RETRO_DEVICE_ID_JOYPAD_L;
		case GenericInputBinding::R1: return RETRO_DEVICE_ID_JOYPAD_R;
		case GenericInputBinding::L2: return RETRO_DEVICE_ID_JOYPAD_L2;
		case GenericInputBinding::R2: return RETRO_DEVICE_ID_JOYPAD_R2;
		case GenericInputBinding::Start: return RETRO_DEVICE_ID_JOYPAD_START;
		default: return -1;
	}
}

// Arcade (System 246/256) input: .acgame titles read JVS I/O, not the PS2 pad.
// Feed the retropad into the per-game JVS layout tables (ACJV selects them from
// the gameid — TEKKEN/STANDARD/SIX_BUTTON). Fixed system buttons:
//   SELECT = insert coin (edge), START = start,
//   L3 = test menu (toggles the TESTMODE DIP switch, edge),
//   R3 = JVS service button (held).
static void UpdateArcadeInput()
{
	static u32 s_prev_edges[2] = {};

	for (u32 player = 0; player < 2; player++)
	{
		const auto bindings = (player == 0) ? ACJV::GetButtonBindings() : ACJV::GetP2ButtonBindings();
		for (const InputBindingInfo& bi : bindings)
		{
			if (bi.bind_index == JVS_BTN_SERVICE)
				continue; // handled below on R3, not on the table's Select hint
			const int retro_id = (bi.bind_index == JVS_BTN_START) ?
				RETRO_DEVICE_ID_JOYPAD_START : GenericBindingToRetroButton(bi.generic_mapping);
			if (retro_id < 0)
				continue;
			const bool pressed = s_input_state_cb(player, RETRO_DEVICE_JOYPAD, 0, retro_id) != 0;
			ACJV::SetButtonState(player, bi.bind_index, pressed);
		}

		ACJV::SetButtonState(player, JVS_BTN_SERVICE,
			s_input_state_cb(player, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3) != 0);

		const bool coin = s_input_state_cb(player, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT) != 0;
		const bool test = s_input_state_cb(player, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3) != 0;
		if (coin && !(s_prev_edges[player] & 1u))
			ACJV::InsertCoin(player);
		if (test && !(s_prev_edges[player] & 2u))
			ACJV::ToggleDIPSwitchState(0); // DIP 0 = TESTMODE -> service/test menu
		s_prev_edges[player] = (coin ? 1u : 0u) | (test ? 2u : 0u);
	}
}

static void UpdateInput()
{
	if (!s_input_poll_cb || !s_input_state_cb)
		return;

	if (s_frame_width == 0)
		return;

	s_input_poll_cb();

	if (ACJV::enabled)
		UpdateArcadeInput();

	static constexpr std::pair<unsigned, u32> button_map[] = {
		{RETRO_DEVICE_ID_JOYPAD_UP, PadDualshock2::Inputs::PAD_UP},
		{RETRO_DEVICE_ID_JOYPAD_RIGHT, PadDualshock2::Inputs::PAD_RIGHT},
		{RETRO_DEVICE_ID_JOYPAD_DOWN, PadDualshock2::Inputs::PAD_DOWN},
		{RETRO_DEVICE_ID_JOYPAD_LEFT, PadDualshock2::Inputs::PAD_LEFT},
		{RETRO_DEVICE_ID_JOYPAD_X, PadDualshock2::Inputs::PAD_TRIANGLE},
		{RETRO_DEVICE_ID_JOYPAD_A, PadDualshock2::Inputs::PAD_CIRCLE},
		{RETRO_DEVICE_ID_JOYPAD_B, PadDualshock2::Inputs::PAD_CROSS},
		{RETRO_DEVICE_ID_JOYPAD_Y, PadDualshock2::Inputs::PAD_SQUARE},
		{RETRO_DEVICE_ID_JOYPAD_SELECT, PadDualshock2::Inputs::PAD_SELECT},
		{RETRO_DEVICE_ID_JOYPAD_START, PadDualshock2::Inputs::PAD_START},
		{RETRO_DEVICE_ID_JOYPAD_L, PadDualshock2::Inputs::PAD_L1},
		{RETRO_DEVICE_ID_JOYPAD_L2, PadDualshock2::Inputs::PAD_L2},
		{RETRO_DEVICE_ID_JOYPAD_R, PadDualshock2::Inputs::PAD_R1},
		{RETRO_DEVICE_ID_JOYPAD_R2, PadDualshock2::Inputs::PAD_R2},
		{RETRO_DEVICE_ID_JOYPAD_L3, PadDualshock2::Inputs::PAD_L3},
		{RETRO_DEVICE_ID_JOYPAD_R3, PadDualshock2::Inputs::PAD_R3},
	};

	for (u32 port = 0; port < static_cast<u32>(s_pad_map.size()); port++)
	{
		const u32 pad = s_pad_map[port];

		for (const auto& [retro_id, ds2_bind] : button_map)
		{
			const int16_t state = s_input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, retro_id);
			Pad::SetControllerState(pad, ds2_bind, state ? 1.0f : 0.0f);
		}

		static constexpr auto axis_value = [](int16_t v, bool negative) {
			const float f = static_cast<float>(v) / 32767.0f;
			return negative ? std::max(-f, 0.0f) : std::max(f, 0.0f);
		};

		const int16_t lx = s_input_state_cb(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X);
		const int16_t ly = s_input_state_cb(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y);
		const int16_t rx = s_input_state_cb(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X);
		const int16_t ry = s_input_state_cb(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y);

		Pad::SetControllerState(pad, PadDualshock2::Inputs::PAD_L_LEFT, axis_value(lx, true));
		Pad::SetControllerState(pad, PadDualshock2::Inputs::PAD_L_RIGHT, axis_value(lx, false));
		Pad::SetControllerState(pad, PadDualshock2::Inputs::PAD_L_UP, axis_value(ly, true));
		Pad::SetControllerState(pad, PadDualshock2::Inputs::PAD_L_DOWN, axis_value(ly, false));
		Pad::SetControllerState(pad, PadDualshock2::Inputs::PAD_R_LEFT, axis_value(rx, true));
		Pad::SetControllerState(pad, PadDualshock2::Inputs::PAD_R_RIGHT, axis_value(rx, false));
		Pad::SetControllerState(pad, PadDualshock2::Inputs::PAD_R_UP, axis_value(ry, true));
		Pad::SetControllerState(pad, PadDualshock2::Inputs::PAD_R_DOWN, axis_value(ry, false));
	}
}

static void OutputAudio()
{
	if (!s_audio_batch_cb)
		return;

	static float float_buffer[MAX_AUDIO_FRAMES_PER_RUN * 2];
	static int16_t s16_buffer[MAX_AUDIO_FRAMES_PER_RUN * 2];

	u32 frames = 0;
	if (s_audio_stream)
		frames = s_audio_stream->PullFrames(float_buffer, MAX_AUDIO_FRAMES_PER_RUN);

	if (frames == 0)
	{
		std::memset(s16_buffer, 0, (SAMPLE_RATE / 60) * 2 * sizeof(int16_t));
		s_audio_batch_cb(s16_buffer, SAMPLE_RATE / 60);
		return;
	}

	for (u32 i = 0; i < frames * 2; i++)
	{
		const float v = std::clamp(float_buffer[i], -1.0f, 1.0f);
		s16_buffer[i] = static_cast<int16_t>(v * 32767.0f);
	}
	s_audio_batch_cb(s16_buffer, frames);
	s_audio_frames_output.fetch_add(frames, std::memory_order_relaxed);
}

static void UpdateAVInfoIfChanged()
{
	static u32 last_fps_bits = 0;
	static u32 last_sample_rate = 0;
	static u32 last_width = 0;
	static u32 last_height = 0;
	static u32 last_aspect_bits = 0;

	const u32 fps_bits = s_vm_fps_bits.load(std::memory_order_acquire);
	const u32 sample_rate = s_audio_sample_rate.load(std::memory_order_acquire);
	const u32 width = s_out_width.load(std::memory_order_acquire);
	const u32 height = s_out_height.load(std::memory_order_acquire);
	const u32 aspect_bits = s_aspect_bits.load(std::memory_order_acquire);

	if (fps_bits == last_fps_bits && sample_rate == last_sample_rate && width == last_width &&
		height == last_height && aspect_bits == last_aspect_bits)
		return;

	if (fps_bits == 0)
		return;

	last_fps_bits = fps_bits;
	last_sample_rate = sample_rate;
	last_width = width;
	last_height = height;
	last_aspect_bits = aspect_bits;

	retro_system_av_info av_info;
	retro_get_system_av_info(&av_info);
	s_environ_cb(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &av_info);
	INFO_LOG("libretro av_info: {}x{} @ {:.2f}Hz, {}Hz audio", av_info.geometry.base_width,
		av_info.geometry.base_height, av_info.timing.fps, sample_rate);
}

void retro_run(void)
{
	bool options_updated = false;
	if (s_environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &options_updated) && options_updated &&
		s_running.load(std::memory_order_acquire))
	{
		ReadCoreOptions(false);
		Host::RunOnCPUThread([]() { VMManager::ApplySettings(); });
	}

	UpdateAVInfoIfChanged();

	if (!s_memory_map_sent && eeMem && s_running.load(std::memory_order_acquire))
	{
		static retro_memory_descriptor descs[2];
		descs[0] = {RETRO_MEMDESC_SYSTEM_RAM, eeMem->Main, 0, 0x00000000u, 0, 0, Ps2MemSize::MainRam, "EE RAM"};
		descs[1] = {RETRO_MEMDESC_SYSTEM_RAM, eeMem->Scratch, 0, 0x70000000u, 0, 0, sizeof(eeMem->Scratch), "Scratchpad"};
		retro_memory_map mmap = {descs, 2};
		s_memory_map_sent = s_environ_cb(RETRO_ENVIRONMENT_SET_MEMORY_MAPS, &mmap);
	}

	if (s_running.load(std::memory_order_acquire))
		UpdateInput();

	if (s_rumble_interface.set_rumble_state)
	{
		for (u32 port = 0; port < static_cast<u32>(s_pad_map.size()); port++)
		{
			const u32 packed = s_rumble_enabled ? s_pad_rumble[s_pad_map[port]].load(std::memory_order_relaxed) : 0;
			s_rumble_interface.set_rumble_state(port, RETRO_RUMBLE_STRONG, static_cast<u16>(packed >> 16));
			s_rumble_interface.set_rumble_state(port, RETRO_RUMBLE_WEAK, static_cast<u16>(packed & 0xFFFF));
		}
	}

	if (!s_running.load(std::memory_order_acquire))
	{
		static std::vector<u32> black(DEFAULT_WIDTH * DEFAULT_HEIGHT, 0);
		if (s_video_cb)
			s_video_cb(black.data(), DEFAULT_WIDTH, DEFAULT_HEIGHT, DEFAULT_WIDTH * sizeof(u32));
		return;
	}

	std::unique_lock lock(s_frame_mutex);
	s_run_token = true;
	s_frame_cv.notify_all();

	// Use a generous timeout to handle slow emulation (JIT cold start, no vs_expand, etc.).
	// At 5fps each PS2 frame takes ~200ms; the old 200ms limit caused constant timeouts → black screen.
	const bool got_frame = s_frame_cv.wait_for(lock, std::chrono::milliseconds(2000), []() { return s_frame_ready; });
	s_frame_ready = false;

	if (got_frame && s_frame_width > 0 && s_frame_height > 0 && s_video_cb)
	{
		s_video_cb(s_frame_pixels.data(), s_frame_width, s_frame_height, s_frame_width * sizeof(u32));
	}
	else if (s_video_cb)
	{
		s_video_cb(nullptr, s_frame_width ? s_frame_width : DEFAULT_WIDTH,
			s_frame_height ? s_frame_height : DEFAULT_HEIGHT,
			(s_frame_width ? s_frame_width : DEFAULT_WIDTH) * sizeof(u32));
	}

	OutputAudio();
}

static constexpr size_t SERIALIZE_BUFFER_SIZE = 96 * 1024 * 1024;
static constexpr u32 SERIALIZE_MAGIC = 0x50325253; // 'P2RS'

struct SerializeHeader
{
	u32 magic;
	u32 reserved;
	u64 zip_size;
};

size_t retro_serialize_size(void)
{
	return SERIALIZE_BUFFER_SIZE;
}

bool retro_serialize(void* data, size_t size)
{
	if (!s_running.load(std::memory_order_acquire) || size < sizeof(SerializeHeader))
		return false;

	bool result = false;
	Host::RunOnCPUThread(
		[data, size, &result]() {
			if (VMManager::GetState() != VMState::Running && VMManager::GetState() != VMState::Paused)
				return;

			Error error;
			std::unique_ptr<ArchiveEntryList> entries = SaveState_DownloadState(&error);
			if (!entries)
			{
				ERROR_LOG("retro_serialize: DownloadState failed: {}", error.GetDescription());
				return;
			}

			std::vector<u8> buffer;
			if (!SaveState_ZipToBuffer(std::move(entries), &buffer, &error))
			{
				ERROR_LOG("retro_serialize: ZipToBuffer failed: {}", error.GetDescription());
				return;
			}

			if (sizeof(SerializeHeader) + buffer.size() > size)
			{
				ERROR_LOG("retro_serialize: state too large ({} bytes)", buffer.size());
				return;
			}

			SerializeHeader header = {SERIALIZE_MAGIC, 0, buffer.size()};
			std::memcpy(data, &header, sizeof(header));
			std::memcpy(static_cast<u8*>(data) + sizeof(header), buffer.data(), buffer.size());
			result = true;
		},
		true);

	return result;
}

bool retro_unserialize(const void* data, size_t size)
{
	if (!s_running.load(std::memory_order_acquire) || size < sizeof(SerializeHeader))
		return false;

	SerializeHeader header;
	std::memcpy(&header, data, sizeof(header));
	if (header.magic != SERIALIZE_MAGIC || sizeof(SerializeHeader) + header.zip_size > size)
		return false;

	bool result = false;
	Host::RunOnCPUThread(
		[data, &header, &result]() {
			if (VMManager::GetState() != VMState::Running && VMManager::GetState() != VMState::Paused)
				return;

			Error error;
			if (!SaveState_UnzipFromBuffer(
					static_cast<const u8*>(data) + sizeof(SerializeHeader), header.zip_size, &error))
			{
				ERROR_LOG("retro_unserialize: {}", error.GetDescription());
				return;
			}

			result = true;
		},
		true);

	return result;
}

void retro_cheat_reset(void) {}
void retro_cheat_set(unsigned index, bool enabled, const char* code) {}

unsigned retro_get_region(void)
{
	const u32 fps_bits = s_vm_fps_bits.load(std::memory_order_acquire);
	return (fps_bits && std::bit_cast<float>(fps_bits) < 55.0f) ? RETRO_REGION_PAL : RETRO_REGION_NTSC;
}

void retro_set_controller_port_device(unsigned port, unsigned device) {}

void* retro_get_memory_data(unsigned id)
{
	if (id == RETRO_MEMORY_SYSTEM_RAM && eeMem && s_running.load(std::memory_order_acquire))
		return eeMem->Main;
	return nullptr;
}

size_t retro_get_memory_size(unsigned id)
{
	if (id == RETRO_MEMORY_SYSTEM_RAM)
		return Ps2MemSize::MainRam;
	return 0;
}

//////////////////////////////////////////////////////////////////////////
// Host implementation
//////////////////////////////////////////////////////////////////////////

void Host::CommitBaseSettingChanges()
{
}

void Host::LoadSettings(SettingsInterface& si, std::unique_lock<std::mutex>& lock)
{
}

void Host::CheckForSettingsChanges(const Pcsx2Config& old_config)
{
}

bool Host::RequestResetSettings(bool folders, bool core, bool controllers, bool hotkeys, bool ui)
{
	return false;
}

void Host::SetDefaultUISettings(SettingsInterface& si)
{
}

bool Host::LocaleCircleConfirm()
{
	return false;
}

std::unique_ptr<ProgressCallback> Host::CreateHostProgressCallback()
{
	return ProgressCallback::CreateNullProgressCallback();
}

void Host::ReportInfoAsync(const std::string_view title, const std::string_view message)
{
	if (!title.empty() && !message.empty())
		INFO_LOG("ReportInfoAsync: {}: {}", title, message);
	else if (!message.empty())
		INFO_LOG("ReportInfoAsync: {}", message);
}

void Host::ReportErrorAsync(const std::string_view title, const std::string_view message)
{
	if (!title.empty() && !message.empty())
		ERROR_LOG("ReportErrorAsync: {}: {}", title, message);
	else if (!message.empty())
		ERROR_LOG("ReportErrorAsync: {}", message);
}

void Host::OpenURL(const std::string_view url)
{
}

bool Host::CopyTextToClipboard(const std::string_view text)
{
	return false;
}

void Host::BeginTextInput()
{
}

void Host::EndTextInput()
{
}

std::optional<WindowInfo> Host::GetTopLevelWindowInfo()
{
	WindowInfo wi;
	wi.type = WindowInfo::Type::Surfaceless;
	wi.surface_width = DEFAULT_WIDTH;
	wi.surface_height = DEFAULT_HEIGHT;
	wi.surface_scale = 1.0f;
	return wi;
}

void Host::OnInputDeviceConnected(const std::string_view identifier, const std::string_view device_name)
{
}

void Host::OnInputDeviceDisconnected(const InputBindingKey key, const std::string_view identifier)
{
}

void Host::SetMouseMode(bool relative_mode, bool hide_cursor)
{
}

void Host::SetMouseLock(bool state)
{
}

std::optional<WindowInfo> Host::AcquireRenderWindow(bool recreate_window)
{
	WindowInfo wi;
	wi.type = WindowInfo::Type::Surfaceless;
	wi.surface_width = DEFAULT_WIDTH;
	wi.surface_height = DEFAULT_HEIGHT;
	wi.surface_scale = 1.0f;
	return wi;
}

void Host::ReleaseRenderWindow()
{
}

void Host::BeginPresentFrame()
{
}

void Host::RequestResizeHostDisplay(s32 width, s32 height)
{
}

void Host::OnVMStarting()
{
}

void Host::OnVMStarted()
{
}

void Host::OnVMDestroyed()
{
}

void Host::OnVMPaused()
{
}

void Host::OnVMResumed()
{
}

void Host::OnGameChanged(const std::string& title, const std::string& elf_override, const std::string& disc_path,
	const std::string& disc_serial, u32 disc_crc, u32 current_crc)
{
	INFO_LOG("Game changed: {} ({})", title, disc_serial);
}

// Defined in pcsx2/arm64/aR5900.cpp (ARM64 build only) — formats the execution-weighted
// interpreter-fallback histogram for the interval into the buffer and resets it.
#ifdef ARCH_ARM64
void recPerfDumpExecFallbacks(char* out, size_t n);
#endif

void Host::OnPerformanceMetricsUpdated()
{
	// Per-thread load report every ~5s (metrics update every 0.5s) — bottleneck
	// hunting: attract mode runs under 60fps on RPi5 and MTVU made no difference,
	// so the limiter is EE or GS, not VU1.
	static int s_perf_log_divider = 0;
	if ((++s_perf_log_divider % 10) != 0)
		return;
	INFO_LOG("perf: {:.1f} fps | EE {:.0f}% GS {:.0f}% VU {:.0f}% | GPU {:.0f}%",
		PerformanceMetrics::GetFPS(), PerformanceMetrics::GetCPUThreadUsage(),
		PerformanceMetrics::GetGSThreadUsage(), PerformanceMetrics::GetVUThreadUsage(),
		PerformanceMetrics::GetGPUUsage());
#ifdef ARCH_ARM64
	char fb[256];
	recPerfDumpExecFallbacks(fb, sizeof(fb));
	INFO_LOG("{}", fb);
#endif
}

void Host::OnSaveStateLoading(const std::string_view filename)
{
}

void Host::OnSaveStateLoaded(const std::string_view filename, bool was_successful)
{
}

void Host::OnSaveStateSaved(const std::string_view filename)
{
}

void Host::RunOnCPUThread(std::function<void()> function, bool block /* = false */)
{
	if (std::this_thread::get_id() == s_cpu_thread_id)
	{
		function();
		return;
	}

	if (!block)
	{
		{
			std::unique_lock lock(s_cpu_work_mutex);
			s_cpu_work.push_back(std::move(function));
			s_cpu_work_pending.store(true, std::memory_order_release);
		}
		s_frame_cv.notify_all();
		return;
	}

	std::mutex done_mutex;
	std::condition_variable done_cv;
	bool done = false;
	{
		std::unique_lock lock(s_cpu_work_mutex);
		s_cpu_work.push_back([&]() {
			function();
			std::unique_lock dlock(done_mutex);
			done = true;
			done_cv.notify_all();
		});
		s_cpu_work_pending.store(true, std::memory_order_release);
	}
	s_frame_cv.notify_all();
	std::unique_lock dlock(done_mutex);
	done_cv.wait(dlock, [&]() { return done; });
}

void Host::RunOnGSThread(std::function<void()> function)
{
	MTGS::RunOnGSThread(std::move(function));
}

void Host::RefreshGameListAsync(bool invalidate_cache)
{
}

void Host::CancelGameListRefresh()
{
}

bool Host::IsFullscreen()
{
	return false;
}

void Host::SetFullscreen(bool enabled)
{
}

void Host::OnCaptureStarted(const std::string& filename)
{
}

void Host::OnCaptureStopped()
{
}

void Host::RequestExitApplication(bool allow_confirm)
{
	VMManager::SetState(VMState::Stopping);
}

void Host::RequestExitBigPicture()
{
}

void Host::RequestVMShutdown(bool allow_confirm, bool allow_save_state, bool default_save_state)
{
	VMManager::SetState(VMState::Stopping);
}

void Host::OnAchievementsLoginSuccess(const char* username, u32 points, u32 sc_points, u32 unread_messages)
{
}

void Host::OnAchievementsLoginRequested(Achievements::LoginRequestReason reason)
{
}

void Host::OnAchievementsHardcoreModeChanged(bool enabled)
{
}

void Host::OnAchievementsRefreshed()
{
}

void Host::OnCoverDownloaderOpenRequested()
{
}

void Host::OnCreateMemoryCardOpenRequested()
{
}

bool Host::InBatchMode()
{
	return true;
}

bool Host::InNoGUIMode()
{
	return true;
}

bool Host::ShouldPreferHostFileSelector()
{
	return false;
}

void Host::OpenHostFileSelectorAsync(std::string_view title, bool select_directory, FileSelectorCallback callback,
	FileSelectorFilters filters, std::string_view initial_directory)
{
	callback(std::string());
}

int Host::LocaleSensitiveCompare(std::string_view lhs, std::string_view rhs)
{
	const int res = std::strncmp(lhs.data(), rhs.data(), std::min(lhs.size(), rhs.size()));
	if (res != 0)
		return res;
	return lhs.size() > rhs.size() ? 1 : (lhs.size() < rhs.size() ? -1 : 0);
}

void Host::PumpMessagesOnCPUThread()
{
	DrainCPUWork();

	if (!s_running.load(std::memory_order_acquire))
		return;

	const float fps = VMManager::GetFrameRate();
	if (fps > 0.0f)
		s_vm_fps_bits.store(std::bit_cast<u32>(fps), std::memory_order_release);

	std::unique_lock lock(s_frame_mutex);
	s_frame_ready = true;
	s_frame_cv.notify_all();
	for (;;)
	{
		s_frame_cv.wait(lock, []() {
			return s_run_token || s_cpu_work_pending.load(std::memory_order_acquire) ||
				   !s_running.load(std::memory_order_acquire);
		});

		if (s_cpu_work_pending.load(std::memory_order_acquire))
		{
			lock.unlock();
			DrainCPUWork();
			lock.lock();
			continue;
		}

		break;
	}
	s_run_token = false;
}

s32 Host::Internal::GetTranslatedStringImpl(
	const std::string_view context, const std::string_view msg, char* tbuf, size_t tbuf_space)
{
	if (msg.size() > tbuf_space)
		return -1;
	else if (msg.empty())
		return 0;

	std::memcpy(tbuf, msg.data(), msg.size());
	return static_cast<s32>(msg.size());
}

std::string Host::TranslatePluralToString(const char* context, const char* msg, const char* disambiguation, int count)
{
	TinyString count_str = TinyString::from_format("{}", count);

	std::string ret(msg);
	for (;;)
	{
		std::string::size_type pos = ret.find("%n");
		if (pos == std::string::npos)
			break;
		ret.replace(pos, pos + 2, count_str.view());
	}

	return ret;
}

std::optional<u32> InputManager::ConvertHostKeyboardStringToCode(const std::string_view str)
{
	return std::nullopt;
}

std::optional<std::string> InputManager::ConvertHostKeyboardCodeToString(u32 code)
{
	return std::nullopt;
}

const char* InputManager::ConvertHostKeyboardCodeToIcon(u32 code)
{
	return nullptr;
}

BEGIN_HOTKEY_LIST(g_host_hotkeys)
END_HOTKEY_LIST()
