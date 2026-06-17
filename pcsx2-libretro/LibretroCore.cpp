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

#if defined(__aarch64__)
#include <arm_neon.h> // NEON-accelerated RGBA->XRGB readback swizzle
#elif defined(__x86_64__) || defined(_M_X64)
#include <tmmintrin.h> // SSSE3 pshufb for the RGBA->XRGB readback swizzle
#endif

// Zero-copy HW render (experimental): import the GS dmabuf into the frontend GL context and
// blit it into the libretro framebuffer. GL via glad (loaded from hw_render.get_proc_address),
// EGL import via eglGetProcAddress (dlsym'd from libEGL).
#include "glad/gl.h"
#include "glad/egl.h"
#include <dlfcn.h>


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
#include "pcsx2/GS/Renderers/OpenGL/GLContextGLX.h" // SetShareContext for zero-copy HW render
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

	// Experimental libretro hardware-render present path (zero-copy). When enabled and
	// the frontend grants a GLES3 HW context, the GL frame is blitted straight into the
	// frontend FBO instead of the GPU->CPU readback + swizzle + re-upload. Wired in
	// stages; see retro_load_game / HWRenderContextReset. Default off -> readback path.
	static retro_hw_render_callback s_hw_render = {};
	static bool s_hw_render_requested = false;
	static std::atomic_bool s_hw_render_active{false};

	// Zero-copy dmabuf present: the GS thread hands a dmabuf fd (+ layout) for the composited
	// frame RT; the frontend thread imports it once and blits the aliasing texture each frame.
	struct DmaBufDesc
	{
		int fd = -1;
		u32 width = 0, height = 0, stride = 0, offset = 0, fourcc = 0;
		u64 modifier = 0;
		bool dirty = false; // a new fd is waiting to be imported
	};
	static std::mutex s_dmabuf_mutex;
	static DmaBufDesc s_dmabuf;
	// blitter state — all touched only on the frontend thread (retro_run / context_reset)
	static bool s_blit_gl_loaded = false;
	static GLuint s_blit_prog = 0;
	static GLuint s_blit_vao = 0;
	static GLuint s_blit_tex = 0;
	static u32 s_blit_w = 0, s_blit_h = 0;
	static EGLImageKHR s_blit_image = EGL_NO_IMAGE_KHR;
	// EGL import entry points (dlsym'd from libEGL; the GL HW context can't be assumed to
	// expose them through get_proc_address)
	static PFNEGLCREATEIMAGEKHRPROC s_eglCreateImageKHR = nullptr;
	static PFNEGLDESTROYIMAGEKHRPROC s_eglDestroyImageKHR = nullptr;
	static EGLDisplay (*s_eglGetCurrentDisplay)() = nullptr;
	static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC s_glEGLImageTargetTexture2DOES = nullptr;

	// Zero-copy via a shared GLX context (desktop X11, e.g. NVIDIA where dmabuf export is
	// unavailable): the GS hands the GL texture id of the composited frame; the frontend's
	// GL context shares the GS context's object namespace, so it samples it directly.
	static std::atomic_bool s_hw_shared_tex_mode{false}; // shared-context present is active
	static std::atomic_bool s_hw_share_ready{false}; // context_reset has run (share set or not)
	static std::mutex s_shared_tex_mutex;
	static u32 s_shared_tex_id = 0, s_shared_tex_w = 0, s_shared_tex_h = 0;
	static void* s_shared_tex_fence = nullptr; // GLsync the frontend waits on before sampling
	static bool s_gl_blit_loaded = false; // desktop-GL blit program (separate from the GLES one)
	static GLuint s_gl_blit_prog = 0, s_gl_blit_vao = 0;

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
		// RGBA8 (driver readback) -> XRGB8888 (libretro): swap the R and B bytes, keep G and
		// the high byte. Runs on the GS thread for the whole upscaled frame each present, so
		// vectorise it on ARM64 (V3D readback is up to 1280x960 = ~1.2M px/frame).
		for (u32 y = 0; y < height; y++)
		{
			const u32* src = pixels + static_cast<size_t>(y) * pitch_px;
			u32* dst = s_frame_pixels.data() + static_cast<size_t>(y) * width;
			u32 x = 0;
#if defined(__aarch64__)
			// NEON: swap byte 0<->2 within each 32-bit pixel, 4 pixels per iteration.
			static const uint8_t s_rb_swap_idx[16] = {2, 1, 0, 3, 6, 5, 4, 7, 10, 9, 8, 11, 14, 13, 12, 15};
			const uint8x16_t idx = vld1q_u8(s_rb_swap_idx);
			for (; x + 4 <= width; x += 4)
			{
				const uint8x16_t v = vld1q_u8(reinterpret_cast<const uint8_t*>(src + x));
				vst1q_u8(reinterpret_cast<uint8_t*>(dst + x), vqtbl1q_u8(v, idx));
			}
#elif defined(__x86_64__) || defined(_M_X64)
			// SSSE3 pshufb: same byte swap, 4 pixels per iteration (desktop, the default path).
			const __m128i idx = _mm_setr_epi8(2, 1, 0, 3, 6, 5, 4, 7, 10, 9, 8, 11, 14, 13, 12, 15);
			for (; x + 4 <= width; x += 4)
			{
				const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + x));
				_mm_storeu_si128(reinterpret_cast<__m128i*>(dst + x), _mm_shuffle_epi8(v, idx));
			}
#endif
			for (; x < width; x++)
			{
				const u32 px = src[x];
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
#if defined(__aarch64__)
	// Recalbox/KMS: the frontend's pacing can be loose (CRT switchres at 59.83/60Hz, vsync off),
	// so PCSX2's own frame limiter throttles to the game's nominal rate (59.94 NTSC / 50 PAL) to
	// stop the VM outrunning real time — that's what produced the >60fps overshoot.
	s_settings_interface.SetBoolValue("EmuCore/GS", "FrameLimitEnable", true);
#else
	// Desktop: the EE/GS free-run (no per-frame token handshake — see PumpMessagesOnCPUThread) and
	// retro_run just presents the latest frame, so the internal limiter paces the EE to the game's
	// nominal rate. Its sleep is on the EE thread, NOT inside retro_run, so it does NOT
	// double-throttle against RA's vsync; retro_run never blocks -> never misses a vblank -> 60.
	// Applies to both present paths (shared-texture zero-copy and the readback fallback).
	s_settings_interface.SetBoolValue("EmuCore/GS", "FrameLimitEnable", true);
#endif
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
		{"pcsx2_hw_download_mode", "HW Download Mode", nullptr,
			"Controls GPU->CPU framebuffer readbacks. 'Disable Readbacks' avoids pipeline stalls on weak, "
			"tiled GPUs (e.g. RPi5/V3D) for a large speedup, but breaks effects that read back the "
			"framebuffer. Hardware renderers only.",
			nullptr, "performance",
			{{"0", "Accurate (Default)"}, {"1", "Disable Readbacks (Faster)"}, {"2", "Unsynchronized"},
				{"3", "Disabled (Fastest, breaks more)"}, {nullptr, nullptr}},
			"0"},
		{"pcsx2_skipdraw", "Skipdraw (Skip Draw Range)", nullptr,
			"Skips the first N draw calls each frame to remove expensive effects (bloom, shadows, etc.) "
			"for a GPU speedup. Game-specific; higher skips more and may remove wanted graphics. "
			"Non-zero enables manual HW fixes, which disables per-game GameDB auto-fixes.",
			nullptr, "performance",
			{{"0", "Disabled (Default)"}, {"1", "1"}, {"2", "2"}, {"3", "3"}, {"4", "4"}, {"5", "5"},
				{"6", "6"}, {"8", "8"}, {"10", "10"}, {nullptr, nullptr}},
			"0"},
		{"pcsx2_disable_framebuffer_fetch", "Disable Framebuffer Fetch", nullptr,
			"Diagnostic: disables shader framebuffer-fetch software blending and uses the RT-copy path "
			"instead. On buggy tiled drivers (e.g. RPi5/V3D) fbfetch can corrupt alpha blending "
			"(opaque shadows, texture flicker). Hardware (OpenGL) renderer only.",
			nullptr, "performance",
			{{"disabled", "Disabled (Default)"}, {"enabled", "Enabled (use RT copy)"}, {nullptr, nullptr}},
			"disabled"},
		{"pcsx2_hw_render", "Hardware Render (zero-copy, experimental)", nullptr,
			"EXPERIMENTAL: present the GL frame directly through the libretro hardware-render context "
			"(zero-copy) instead of the GPU->CPU readback path. On desktop this also decouples emulation "
			"from the present for stable 60fps pacing; on RPi5/V3D it drops the per-frame readback + CPU "
			"swizzle + re-upload. Validated on NVIDIA/X11; AMD/Intel/Wayland untested. Requires a restart. "
			"OpenGL renderer only; falls back to readback if the frontend has no HW context.",
			nullptr, "performance",
			{{"disabled", "Disabled (Default)"}, {"enabled", "Enabled (experimental)"}, {nullptr, nullptr}},
			"disabled"},
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
	s_settings_interface.SetBoolValue("EmuCore/GS", "DisableFramebufferFetch",
		std::strcmp(get_option("pcsx2_disable_framebuffer_fetch", "disabled"), "enabled") == 0);

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

	// GPU->CPU readback control. Not masked by ManualUserHacks, so it always applies.
	s_settings_interface.SetIntValue("EmuCore/GS", "HWDownloadMode", get_int_option("pcsx2_hw_download_mode", "0"));

	// Skipdraw: skip draw calls [1, N] each frame. SkipDraw is zeroed by GSOptions::MaskUserHacks()
	// unless ManualUserHacks ("UserHacks") is set, so only enable that master toggle when skipdraw
	// is actually requested — otherwise the per-game GameDB auto-fixes stay active.
	const int skipdraw = get_int_option("pcsx2_skipdraw", "0");
	if (skipdraw > 0)
	{
		s_settings_interface.SetBoolValue("EmuCore/GS", "UserHacks", true);
		s_settings_interface.SetIntValue("EmuCore/GS", "UserHacks_SkipDraw_Start", 1);
		s_settings_interface.SetIntValue("EmuCore/GS", "UserHacks_SkipDraw_End", skipdraw);
	}
	else
	{
		s_settings_interface.SetIntValue("EmuCore/GS", "UserHacks_SkipDraw_Start", 0);
		s_settings_interface.SetIntValue("EmuCore/GS", "UserHacks_SkipDraw_End", 0);
	}

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

#if !defined(__aarch64__)
			// Desktop zero-copy HW render: the GS context must be created sharing the frontend's
			// GLX context, captured in context_reset on the frontend thread. Wait for it before
			// opening the GS. Bounded so a frontend that never resets the context (or isn't on
			// GLX) still boots — it just falls back to the readback present.
			if (s_hw_render_requested)
			{
				std::unique_lock lk(s_session_mutex);
				s_session_cv.wait_for(lk, std::chrono::seconds(3),
					[]() { return s_hw_share_ready.load(std::memory_order_acquire); });
			}
#endif

			Error vm_error;
			if (VMManager::Initialize(s_boot_params, &vm_error) == VMBootResult::StartupSuccess)
			{
				VMManager::SetState(VMState::Running);
#if defined(__aarch64__)
				// Recalbox/KMS: core throttles to the game's nominal rate (loose frontend pacing).
				VMManager::SetLimiterMode(LimiterModeType::Nominal);
#else
				// Desktop: the EE free-runs (no token handshake on either present path), so the
				// internal limiter paces it to the nominal rate — its sleep is on this thread, not
				// in retro_run, so no double-throttle against RA's vsync.
				VMManager::SetLimiterMode(LimiterModeType::Nominal);
#endif
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

// Reads a core option from anywhere (the ReadCoreOptions lambda is local to it).
static const char* GetCoreOption(const char* key, const char* fallback)
{
	retro_variable var{key, nullptr};
	if (s_environ_cb && s_environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		return var.value;
	return fallback;
}

// libretro HW render callbacks. Invoked on the FRONTEND thread with the libretro
// GLES3 context current — this is where the zero-copy present resources live (and,
// later, where the frontend EGL context is captured to share with the GS context).
static void HWRenderContextReset()
{
	INFO_LOG("HW render: context reset — frontend HW context is current on the frontend thread.");
	s_hw_render_active.store(true, std::memory_order_release);
	// The GL context was (re)created: our blitter objects + imported texture are gone. Rebuild
	// lazily, and ask the GS to re-export so we get a fresh dmabuf fd to import into the new ctx.
	s_blit_gl_loaded = false;
	s_blit_prog = s_blit_vao = s_blit_tex = 0;
	s_blit_image = EGL_NO_IMAGE_KHR;
	s_blit_w = s_blit_h = 0;
	GSForceDMABUFReexport();

#if !defined(__aarch64__)
	// Desktop zero-copy: capture the frontend's GLX context so the GS creates its context
	// sharing it (the composited texture then lives in a shared object namespace). The GS open
	// on the CPU thread waits for s_hw_share_ready, so this must run before it (it does: the GS
	// open is gated, and RetroArch calls context_reset on the first retro_run).
	s_gl_blit_loaded = false;
	s_gl_blit_prog = s_gl_blit_vao = 0;
	{
		void* gl = dlopen("libGL.so.1", RTLD_NOW | RTLD_GLOBAL);
		void* ctx = nullptr;
		void* dpy = nullptr;
		if (gl)
		{
			auto get_ctx = reinterpret_cast<void* (*)()>(dlsym(gl, "glXGetCurrentContext"));
			auto get_dpy = reinterpret_cast<void* (*)()>(dlsym(gl, "glXGetCurrentDisplay"));
			ctx = get_ctx ? get_ctx() : nullptr;
			dpy = get_dpy ? get_dpy() : nullptr;
		}
		if (ctx && dpy)
		{
			GLContextGLX::SetShareContext(ctx, dpy);
			s_hw_shared_tex_mode.store(true, std::memory_order_release);
			INFO_LOG("HW render: captured frontend GLX context for zero-copy shared-texture present.");
		}
		else
		{
			// Frontend isn't on GLX (e.g. Wayland/EGL) — can't share. Drop to readback so the
			// GS still produces a picture instead of a black screen.
			Console.Warning("HW render: no frontend GLX context to share; using readback present.");
			s_hw_shared_tex_mode.store(false, std::memory_order_release);
			GSSetFramebufferSharedTextureExport(false);
		}
	}
	{
		std::unique_lock lk(s_session_mutex);
		s_hw_share_ready.store(true, std::memory_order_release);
		s_session_cv.notify_all();
	}
#endif
}

static void HWRenderContextDestroy()
{
	INFO_LOG("HW render: context destroy.");
	s_hw_render_active.store(false, std::memory_order_release);
	// The GL objects belong to the now-destroyed context; forget them so they are rebuilt.
	s_blit_gl_loaded = false;
	s_blit_prog = s_blit_vao = s_blit_tex = 0;
	s_blit_image = EGL_NO_IMAGE_KHR;
	s_blit_w = s_blit_h = 0;
	std::unique_lock lk(s_dmabuf_mutex);
	if (s_dmabuf.fd >= 0)
		close(s_dmabuf.fd);
	s_dmabuf = DmaBufDesc{};
}

// GS thread: a new exported dmabuf (first frame / resize). Stash it for the frontend to import.
static void OnDMABUFFrame(int fd, u32 width, u32 height, u32 stride, u32 offset, u32 fourcc, u64 modifier)
{
	std::unique_lock lk(s_dmabuf_mutex);
	if (s_dmabuf.dirty && s_dmabuf.fd >= 0)
		close(s_dmabuf.fd); // a previous fd never got imported
	s_dmabuf = DmaBufDesc{fd, width, height, stride, offset, fourcc, modifier, true};
}

static GLuint HWCompileShader(GLenum type, const char* src)
{
	GLuint s = glCreateShader(type);
	glShaderSource(s, 1, &src, nullptr);
	glCompileShader(s);
	GLint ok = 0;
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok)
	{
		char log[512] = {};
		glGetShaderInfoLog(s, sizeof(log), nullptr, log);
		Console.WarningFmt("HW blit: shader compile failed: {}", log);
		glDeleteShader(s);
		return 0;
	}
	return s;
}

// Frontend thread: load GL + EGL-import entry points and build the fullscreen blit program. Once.
static bool HWBlitInit()
{
	if (s_blit_gl_loaded)
		return s_blit_prog != 0;
	s_blit_gl_loaded = true;

	if (gladLoadGLES2([](const char* n) { return reinterpret_cast<GLADapiproc>(s_hw_render.get_proc_address(n)); }) == 0)
	{
		Console.Warning("HW blit: gladLoadGLES2 failed.");
		return false;
	}

	void* egl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
	if (!egl)
		egl = dlopen("libEGL.so", RTLD_NOW | RTLD_LOCAL);
	if (egl)
	{
		using GetProc = void* (*)(const char*);
		auto egl_gpa = reinterpret_cast<GetProc>(dlsym(egl, "eglGetProcAddress"));
		s_eglGetCurrentDisplay = reinterpret_cast<EGLDisplay (*)()>(dlsym(egl, "eglGetCurrentDisplay"));
		if (egl_gpa)
		{
			s_eglCreateImageKHR = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(egl_gpa("eglCreateImageKHR"));
			s_eglDestroyImageKHR = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(egl_gpa("eglDestroyImageKHR"));
			s_glEGLImageTargetTexture2DOES =
				reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(egl_gpa("glEGLImageTargetTexture2DOES"));
		}
	}
	if (!s_eglCreateImageKHR || !s_eglDestroyImageKHR || !s_eglGetCurrentDisplay || !s_glEGLImageTargetTexture2DOES)
	{
		Console.WarningFmt("HW blit: missing EGL import entry points (create={}, destroy={}, dpy={}, target={}).",
			s_eglCreateImageKHR != nullptr, s_eglDestroyImageKHR != nullptr,
			s_eglGetCurrentDisplay != nullptr, s_glEGLImageTargetTexture2DOES != nullptr);
		return false;
	}

	static const char* vs =
		"#version 310 es\n"
		"out vec2 v_uv;\n"
		"void main(){vec2 p=vec2(float((gl_VertexID<<1)&2),float(gl_VertexID&2));"
		"v_uv=p;gl_Position=vec4(p*2.0-1.0,0.0,1.0);}\n";
	static const char* fs =
		"#version 310 es\n"
		"precision mediump float;\n"
		"in vec2 v_uv;\nuniform sampler2D u_tex;\nout vec4 o;\n"
		"void main(){o=texture(u_tex,vec2(v_uv.x,1.0-v_uv.y));}\n"; // Y-flip: GS top-left -> GL bottom-left

	GLuint v = HWCompileShader(GL_VERTEX_SHADER, vs);
	GLuint f = HWCompileShader(GL_FRAGMENT_SHADER, fs);
	if (!v || !f)
		return false;
	s_blit_prog = glCreateProgram();
	glAttachShader(s_blit_prog, v);
	glAttachShader(s_blit_prog, f);
	glLinkProgram(s_blit_prog);
	GLint ok = 0;
	glGetProgramiv(s_blit_prog, GL_LINK_STATUS, &ok);
	glDeleteShader(v);
	glDeleteShader(f);
	if (!ok)
	{
		Console.Warning("HW blit: program link failed.");
		glDeleteProgram(s_blit_prog);
		s_blit_prog = 0;
		return false;
	}
	glGenVertexArrays(1, &s_blit_vao);
	glGenTextures(1, &s_blit_tex);
	INFO_LOG("HW blit: initialized (zero-copy dmabuf present).");
	return true;
}

// Frontend thread: import the dmabuf into s_blit_tex (the texture then aliases the GS RT buffer).
static bool HWImportDmaBuf(const DmaBufDesc& d)
{
	if (s_blit_image != EGL_NO_IMAGE_KHR)
	{
		s_eglDestroyImageKHR(s_eglGetCurrentDisplay(), s_blit_image);
		s_blit_image = EGL_NO_IMAGE_KHR;
	}
	// For a LINEAR buffer (modifier 0) omit the modifier attribs entirely: the default import
	// layout is linear, and this avoids relying on EGL_EXT_image_dma_buf_import_modifiers. Only
	// a tiled buffer (modifier != 0) needs the explicit modifier passed through.
	EGLint attribs[20];
	int n = 0;
	attribs[n++] = EGL_WIDTH;                     attribs[n++] = static_cast<EGLint>(d.width);
	attribs[n++] = EGL_HEIGHT;                    attribs[n++] = static_cast<EGLint>(d.height);
	attribs[n++] = EGL_LINUX_DRM_FOURCC_EXT;      attribs[n++] = static_cast<EGLint>(d.fourcc);
	attribs[n++] = EGL_DMA_BUF_PLANE0_FD_EXT;     attribs[n++] = d.fd;
	attribs[n++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT; attribs[n++] = static_cast<EGLint>(d.offset);
	attribs[n++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;  attribs[n++] = static_cast<EGLint>(d.stride);
	if (d.modifier != 0)
	{
		attribs[n++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT; attribs[n++] = static_cast<EGLint>(d.modifier & 0xFFFFFFFFu);
		attribs[n++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT; attribs[n++] = static_cast<EGLint>(d.modifier >> 32);
	}
	attribs[n++] = EGL_NONE;
	s_blit_image = s_eglCreateImageKHR(s_eglGetCurrentDisplay(), EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
		static_cast<EGLClientBuffer>(nullptr), attribs);
	if (s_blit_image == EGL_NO_IMAGE_KHR)
	{
		Console.Warning("HW blit: eglCreateImageKHR(dmabuf) failed.");
		return false;
	}
	glBindTexture(GL_TEXTURE_2D, s_blit_tex);
	s_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, static_cast<GLeglImageOES>(s_blit_image));
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_2D, 0);
	s_blit_w = d.width;
	s_blit_h = d.height;
	INFO_LOG("HW blit: imported dmabuf {}x{}.", d.width, d.height);
	return true;
}

// Frontend thread (retro_run): blit the imported dmabuf into the libretro framebuffer.
// Returns true if it presented (so the caller skips the software path).
static bool HWPresentDMABuf()
{
	if (!s_hw_render_active.load(std::memory_order_acquire) || !HWBlitInit())
		return false;

	DmaBufDesc local;
	{
		std::unique_lock lk(s_dmabuf_mutex);
		if (s_dmabuf.dirty)
		{
			local = s_dmabuf;
			s_dmabuf.dirty = false;
			s_dmabuf.fd = -1; // ownership transferred to `local`
		}
	}
	if (local.dirty && local.fd >= 0)
	{
		HWImportDmaBuf(local);
		close(local.fd); // the EGLImage keeps the buffer alive
	}
	if (s_blit_image == EGL_NO_IMAGE_KHR || s_blit_w == 0)
		return false;

	glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(s_hw_render.get_current_framebuffer()));
	glViewport(0, 0, static_cast<GLsizei>(s_blit_w), static_cast<GLsizei>(s_blit_h));
	glDisable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_CULL_FACE);
	glUseProgram(s_blit_prog);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, s_blit_tex);
	const GLint loc = glGetUniformLocation(s_blit_prog, "u_tex");
	if (loc >= 0)
		glUniform1i(loc, 0);
	glBindVertexArray(s_blit_vao);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glBindVertexArray(0);
	glBindTexture(GL_TEXTURE_2D, 0);

	s_video_cb(RETRO_HW_FRAME_BUFFER_VALID, s_blit_w, s_blit_h, 0);
	return true;
}

#if !defined(__aarch64__)
// GS thread: the composited frame's GL texture id (shared with the frontend's GL context).
static void OnSharedTextureFrame(u32 gl_texture_id, u32 width, u32 height, void* fence)
{
	std::unique_lock lk(s_shared_tex_mutex);
	// Drop a fence the frontend never consumed (frame produced but not presented) so it can't leak.
	// We're on the GS thread whose context shares the namespace, so glDeleteSync is valid here.
	if (s_shared_tex_fence)
		glDeleteSync(static_cast<GLsync>(s_shared_tex_fence));
	s_shared_tex_id = gl_texture_id;
	s_shared_tex_w = width;
	s_shared_tex_h = height;
	s_shared_tex_fence = fence;
}

// Frontend thread: build the desktop-GL fullscreen-triangle blit program (once).
static bool HWBlitInitGL()
{
	if (s_gl_blit_loaded)
		return s_gl_blit_prog != 0;
	s_gl_blit_loaded = true;

	if (gladLoadGL([](const char* n) { return reinterpret_cast<GLADapiproc>(s_hw_render.get_proc_address(n)); }) == 0)
	{
		Console.Warning("HW blit(GL): gladLoadGL failed.");
		return false;
	}

	static const char* vs =
		"#version 330 core\nout vec2 v_uv;\nvoid main(){vec2 p=vec2(float((gl_VertexID<<1)&2),float(gl_VertexID&2));"
		"v_uv=p;gl_Position=vec4(p*2.0-1.0,0.0,1.0);}\n";
	static const char* fs =
		"#version 330 core\nin vec2 v_uv;out vec4 o;uniform sampler2D u_tex;"
		"void main(){o=texture(u_tex,vec2(v_uv.x,1.0-v_uv.y));}\n";

	const GLuint v = HWCompileShader(GL_VERTEX_SHADER, vs);
	const GLuint f = HWCompileShader(GL_FRAGMENT_SHADER, fs);
	if (!v || !f)
		return false;
	s_gl_blit_prog = glCreateProgram();
	glAttachShader(s_gl_blit_prog, v);
	glAttachShader(s_gl_blit_prog, f);
	glLinkProgram(s_gl_blit_prog);
	glDeleteShader(v);
	glDeleteShader(f);
	GLint ok = 0;
	glGetProgramiv(s_gl_blit_prog, GL_LINK_STATUS, &ok);
	if (!ok)
	{
		Console.Warning("HW blit(GL): program link failed.");
		glDeleteProgram(s_gl_blit_prog);
		s_gl_blit_prog = 0;
		return false;
	}
	// Bind the sampler to texture unit 0 once — it's program state, no need to re-set each frame.
	glUseProgram(s_gl_blit_prog);
	const GLint loc = glGetUniformLocation(s_gl_blit_prog, "u_tex");
	if (loc >= 0)
		glUniform1i(loc, 0);
	glUseProgram(0);

	glGenVertexArrays(1, &s_gl_blit_vao);
	return true;
}

// Frontend thread (retro_run): blit the given shared GS texture into the libretro framebuffer.
// The texture id is captured by the caller before the next frame's run token is posted, so the
// GS (now rendering into the other double-buffered RT) can't disturb it. Returns true if presented.
static bool HWPresentSharedTexture(u32 tex, u32 w, u32 h, void* fence)
{
	if (!s_hw_render_active.load(std::memory_order_acquire) || !s_hw_shared_tex_mode.load(std::memory_order_acquire))
	{
		if (fence)
			glDeleteSync(static_cast<GLsync>(fence)); // not presenting; don't leak the sync
		return false;
	}
	if (tex == 0 || !HWBlitInitGL())
	{
		if (fence)
			glDeleteSync(static_cast<GLsync>(fence));
		return false;
	}

	// GPU-side wait: order our sample after the GS render WITHOUT blocking this CPU thread (unlike
	// glFinish). The sync object is shared via the common GL namespace. Deleting it right after
	// queuing the wait is fine — the wait is already in our command stream.
	if (fence)
	{
		glWaitSync(static_cast<GLsync>(fence), 0, GL_TIMEOUT_IGNORED);
		glDeleteSync(static_cast<GLsync>(fence));
	}

	glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(s_hw_render.get_current_framebuffer()));
	glViewport(0, 0, static_cast<GLsizei>(w), static_cast<GLsizei>(h));
	glDisable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_CULL_FACE);
	glUseProgram(s_gl_blit_prog);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glBindVertexArray(s_gl_blit_vao);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glBindVertexArray(0);
	glBindTexture(GL_TEXTURE_2D, 0);

	s_video_cb(RETRO_HW_FRAME_BUFFER_VALID, w, h, 0);
	return true;
}
#endif // !__aarch64__

// Negotiate a libretro hardware-render context when pcsx2_hw_render is enabled. On arm64
// (RPi5/KMS) this is a GLES3 context with the GS frame exported as a dmabuf. On desktop it is
// a desktop-GL context whose GLX context is shared with the GS so the composited texture is
// sampled directly (zero-copy, no dmabuf which NVIDIA's GLX can't export).
static void TryInitHWRender()
{
	s_hw_render_requested = (std::strcmp(GetCoreOption("pcsx2_hw_render", "disabled"), "enabled") == 0);
	if (!s_hw_render_requested)
		return;

	s_hw_render = {};
	s_hw_render.context_reset = &HWRenderContextReset;
	s_hw_render.context_destroy = &HWRenderContextDestroy;
	s_hw_render.bottom_left_origin = true; // GL framebuffers are bottom-left origin
	s_hw_render.depth = false;
	s_hw_render.stencil = false;
	s_hw_render.cache_context = true;

#if defined(__aarch64__)
	// RPi5/KMS: GLES3 + dmabuf export.
	s_hw_render.context_type = RETRO_HW_CONTEXT_OPENGLES3;
	if (!s_environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &s_hw_render))
	{
		Console.Warning("HW render: frontend rejected the GLES3 HW context; falling back to readback.");
		s_hw_render_requested = false;
		return;
	}
	INFO_LOG("HW render: GLES3 HW context requested for zero-copy dmabuf present.");
	GSSetFramebufferDMABUFExport(true);
	GSSetFramebufferDMABUFCallback(&OnDMABUFFrame);
#else
	// Desktop: desktop-GL context, GS GLX context shared with it (zero-copy shared texture).
	s_hw_render.context_type = RETRO_HW_CONTEXT_OPENGL_CORE;
	s_hw_render.version_major = 3;
	s_hw_render.version_minor = 3;
	if (!s_environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &s_hw_render))
	{
		Console.Warning("HW render: frontend rejected the GL core HW context; falling back to readback.");
		s_hw_render_requested = false;
		return;
	}
	INFO_LOG("HW render: GL core HW context requested for zero-copy shared-texture present.");
	GSSetFramebufferSharedTextureExport(true);
	GSSetFramebufferSharedTextureCallback(&OnSharedTextureFrame);
#endif
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

	// Experimental zero-copy present: must be negotiated before the frontend brings up
	// video. No-op (readback path) unless pcsx2_hw_render is enabled.
	TryInitHWRender();

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

	// Gate input on the first produced frame. In dmabuf HW-render mode the readback never
	// runs (so s_frame_width stays 0) — use the HW-render-active flag as the readiness signal.
	if (s_frame_width == 0 && !s_hw_render_active.load(std::memory_order_acquire))
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

#if !defined(__aarch64__)
	// Desktop shared-texture HW render: fully decoupled present. The EE/GS free-run (no per-frame
	// token handshake — see PumpMessagesOnCPUThread), paced by the internal limiter to the nominal
	// rate. retro_run just grabs the latest texture the GS has published and presents it, NEVER
	// blocking — so it always returns early and catches every vblank, instead of the depth-1
	// handshake whose blocking wait beat against RA's vsync and dipped light scenes below 60.
	// Latest-wins: if no new frame was produced, we re-present the previous one (a dup, paced by
	// vsync); if the EE got ahead, we present its newest — exactly like standalone's VPS vs FPS
	// drift. The shared RT is triple-buffered (GSRenderer) so the GS can publish ahead without
	// overwriting the texture we're sampling.
	if (s_hw_shared_tex_mode.load(std::memory_order_acquire))
	{
		lock.unlock(); // no handshake on this path; the frame mutex isn't needed

		u32 cap_tex = 0, cap_w = 0, cap_h = 0;
		void* cap_fence = nullptr;
		{
			std::unique_lock tlk(s_shared_tex_mutex);
			cap_tex = s_shared_tex_id;
			cap_w = s_shared_tex_w;
			cap_h = s_shared_tex_h;
			cap_fence = s_shared_tex_fence;
			s_shared_tex_fence = nullptr; // take ownership: HWPresent deletes it (once per fence)
		}

		// HWPresentSharedTexture checks s_hw_render_active itself and deletes cap_fence on every
		// path, so no fence can leak. cap_tex==0 (no frame yet) -> returns false -> black.
		const bool presented = HWPresentSharedTexture(cap_tex, cap_w, cap_h, cap_fence);
		if (!presented && s_video_cb)
			s_video_cb(nullptr, DEFAULT_WIDTH, DEFAULT_HEIGHT, DEFAULT_WIDTH * sizeof(u32));

		OutputAudio();
		return;
	}
#endif

#if !defined(__aarch64__)
	// Desktop readback present (hw_render off, or shared-texture unavailable e.g. Wayland): same
	// decoupled model as the shared-texture path — the EE/GS free-run paced by the internal limiter
	// and retro_run never blocks; it just presents the latest readback buffer. s_frame_mutex (held
	// here from above) already serialises it against the GS thread's write, so no tearing and no
	// second buffer needed. The old blocking handshake beat RA's vsync and capped this path < 60.
	if (s_frame_width > 0 && s_frame_height > 0 && s_video_cb)
		s_video_cb(s_frame_pixels.data(), s_frame_width, s_frame_height, s_frame_width * sizeof(u32));
	else if (s_video_cb)
		s_video_cb(nullptr, s_frame_width ? s_frame_width : DEFAULT_WIDTH,
			s_frame_height ? s_frame_height : DEFAULT_HEIGHT,
			(s_frame_width ? s_frame_width : DEFAULT_WIDTH) * sizeof(u32));
	lock.unlock();
	OutputAudio();
	return;
#endif

#if defined(__aarch64__)
	// Synchronous path (arm64/Recalbox): token handshake, then readback present or RPi5/KMS dmabuf
	// zero-copy. The frame mutex is held across the present so s_frame_pixels stays stable. The
	// frontend pacing is loose (vsync off / CRT switchres) so the handshake doesn't beat a vblank.
	s_run_token = true;
	s_frame_cv.notify_all();

	// Use a generous timeout to handle slow emulation (JIT cold start, no vs_expand, etc.).
	// At 5fps each PS2 frame takes ~200ms; the old 200ms limit caused constant timeouts → black screen.
	const bool got_frame = s_frame_cv.wait_for(lock, std::chrono::milliseconds(2000), []() { return s_frame_ready; });
	s_frame_ready = false;

	bool hw_presented = false;
	if (got_frame && s_hw_render_active.load(std::memory_order_acquire))
		hw_presented = HWPresentDMABuf(); // RPi5/KMS: imported dmabuf

	if (hw_presented)
	{
		// Presented zero-copy (the present fn called s_video_cb itself).
	}
	else if (got_frame && s_frame_width > 0 && s_frame_height > 0 && s_video_cb)
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
#endif
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
	char fb[512];
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

#if !defined(__aarch64__)
	// Desktop (both present paths): the EE free-runs, paced by the internal limiter (set in the run
	// loop), and retro_run presents whatever frame is latest. Don't block on a per-frame token here
	// — that serial handshake is what beat against RA's vsync and capped fps below 60.
	{
		std::unique_lock lock(s_frame_mutex);
		s_frame_ready = true;
		s_frame_cv.notify_all();
		return;
	}
#endif

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
