// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/OpenGL/GLContextEGL.h"

#include "common/Assertions.h"
#include "common/Console.h"
#include "common/DynamicLibrary.h"
#include "common/Error.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <optional>
#include <vector>

#ifndef _WIN32
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#endif

static DynamicLibrary s_egl_library;
static std::atomic_uint32_t s_egl_refcount = 0;

static bool LoadEGL()
{
	// We're not going to be calling this from multiple threads concurrently.
	// So, not wrapping this in a mutex should be fine.
	if (s_egl_refcount.fetch_add(1, std::memory_order_acq_rel) == 0)
	{
		pxAssert(!s_egl_library.IsOpen());

		std::string egl_libname = DynamicLibrary::GetVersionedFilename("libEGL");
		Console.WriteLnFmt("Loading EGL from {}...", egl_libname);

		Error error;
		if (!s_egl_library.Open(egl_libname.c_str(), &error))
		{
			// Try versioned.
			egl_libname = DynamicLibrary::GetVersionedFilename("libEGL", 1);
			Console.WriteLnFmt("Loading EGL from {}...", egl_libname);
			if (!s_egl_library.Open(egl_libname.c_str(), &error))
				Console.ErrorFmt("Failed to load EGL: {}", error.GetDescription());
		}
	}

	return s_egl_library.IsOpen();
}

static void UnloadEGL()
{
	pxAssert(s_egl_refcount.load(std::memory_order_acquire) > 0);
	if (s_egl_refcount.fetch_sub(1, std::memory_order_acq_rel) == 1)
	{
		Console.WriteLn("Unloading EGL.");
		s_egl_library.Close();
	}
}

static bool LoadGLADEGL(EGLDisplay display, Error* error)
{
	const int version =
		gladLoadEGL(display, [](const char* name) { return (GLADapiproc)s_egl_library.GetSymbolAddress(name); });
	if (version == 0)
	{
		Error::SetStringView(error, "Loading GLAD EGL functions failed");
		return false;
	}

	Console.WriteLnFmt("GLAD EGL Version: {}.{}", GLAD_VERSION_MAJOR(version), GLAD_VERSION_MINOR(version));
	return true;
}

GLContextEGL::GLContextEGL(const WindowInfo& wi)
	: GLContext(wi)
{
	LoadEGL();
}

GLContextEGL::~GLContextEGL()
{
	DestroySurface();
	DestroyContext();

#ifndef _WIN32
	if (m_gbm_device && m_gbm_lib)
	{
		typedef void (*PFN_gbm_device_destroy)(void*);
		auto gbm_destroy = reinterpret_cast<PFN_gbm_device_destroy>(dlsym(m_gbm_lib, "gbm_device_destroy"));
		if (gbm_destroy)
			gbm_destroy(m_gbm_device);
	}
	if (m_gbm_lib)
		dlclose(m_gbm_lib);
	if (m_gbm_fd >= 0)
		close(m_gbm_fd);
#endif

	UnloadEGL();
}

std::unique_ptr<GLContext> GLContextEGL::Create(const WindowInfo& wi, std::span<const Version> versions_to_try,
	Error* error)
{
	std::unique_ptr<GLContextEGL> context = std::make_unique<GLContextEGL>(wi);
	if (!context->Initialize(versions_to_try, error))
		return nullptr;

	return context;
}

bool GLContextEGL::Initialize(std::span<const Version> versions_to_try, Error* error)
{
	if (!LoadGLADEGL(EGL_NO_DISPLAY, error))
		return false;

	m_display = GetPlatformDisplay(error);
	if (m_display == EGL_NO_DISPLAY)
		return false;

	int egl_major, egl_minor;
	if (!eglInitialize(m_display, &egl_major, &egl_minor))
	{
		const int gerror = static_cast<int>(eglGetError());
		Error::SetStringFmt(error, "eglInitialize() failed: {} (0x{:X})", gerror, gerror);
		return false;
	}

	Console.WriteLnFmt("eglInitialize() version: {}.{}", egl_major, egl_minor);

	// Log supported client APIs — if "OpenGL" is absent, only GLES is available.
	const char* client_apis = eglQueryString(m_display, EGL_CLIENT_APIS);
	Console.WriteLnFmt("EGL client APIs: {}", client_apis ? client_apis : "(null)");

	// Re-initialize EGL/GLAD.
	if (!LoadGLADEGL(m_display, error))
		return false;

	if (!GLAD_EGL_KHR_surfaceless_context)
		Console.Warning("EGL implementation does not support surfaceless contexts, emulating with pbuffers");

	for (const Version& cv : versions_to_try)
	{
		if (CreateContextAndSurface(cv, nullptr, true))
			return true;
	}

	Error::SetStringView(error, "Failed to create any context versions");
	return false;
}

EGLDisplay GLContextEGL::GetPlatformDisplay(Error* error)
{
	// For surfaceless contexts (libretro/headless), try GBM first.
	// On KMS/DRM systems (e.g. Recalbox on RPi5) the v3d driver only exposes GLES
	// regardless of whether GBM or EGL_MESA_platform_surfaceless is used.
	// Desktop GL versions will fail quietly; GLES 3.1/3.2 is the fallback.
	if (m_wi.type == WindowInfo::Type::Surfaceless)
	{
		EGLDisplay dpy = TryGBMPlatformDisplay(error);
		if (dpy != EGL_NO_DISPLAY)
			return dpy;
	}

	EGLDisplay dpy = TryGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, "EGL_MESA_platform_surfaceless");
	if (dpy == EGL_NO_DISPLAY)
		dpy = GetFallbackDisplay(error);

	return dpy;
}

EGLDisplay GLContextEGL::TryGBMPlatformDisplay(Error* error)
{
#ifndef _WIN32
	const char* extensions_str = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
	if (!extensions_str || !std::strstr(extensions_str, "EGL_MESA_platform_gbm"))
		return EGL_NO_DISPLAY;

	PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display_ext =
		reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
	if (!get_platform_display_ext)
		return EGL_NO_DISPLAY;

	// Try card0 first: on some drivers (e.g. V3D) the render node (renderD128)
	// only exposes GLES while the full card device exposes desktop OpenGL too.
	static const char* s_drm_nodes[] = {
		"/dev/dri/card0", "/dev/dri/card1",
		"/dev/dri/renderD128", "/dev/dri/renderD129", "/dev/dri/renderD130",
		nullptr};

	for (int i = 0; s_drm_nodes[i]; i++)
	{
		int fd = open(s_drm_nodes[i], O_RDWR);
		if (fd < 0)
			continue;

		void* lib = dlopen("libgbm.so.1", RTLD_NOW | RTLD_LOCAL);
		if (!lib)
		{
			close(fd);
			continue;
		}

		typedef void* (*PFN_gbm_create_device)(int);
		typedef void (*PFN_gbm_device_destroy)(void*);
		auto gbm_create = reinterpret_cast<PFN_gbm_create_device>(dlsym(lib, "gbm_create_device"));
		if (!gbm_create)
		{
			dlclose(lib);
			close(fd);
			continue;
		}

		void* dev = gbm_create(fd);
		if (!dev)
		{
			dlclose(lib);
			close(fd);
			continue;
		}

		EGLDisplay dpy = get_platform_display_ext(EGL_PLATFORM_GBM_MESA, dev, nullptr);
		if (dpy != EGL_NO_DISPLAY)
		{
			Console.WriteLnFmt("Using EGL GBM platform with {}.", s_drm_nodes[i]);
			m_gbm_fd = fd;
			m_gbm_lib = lib;
			m_gbm_device = dev;
			return dpy;
		}

		// This node didn't work; clean up and try the next one.
		auto gbm_destroy = reinterpret_cast<PFN_gbm_device_destroy>(dlsym(lib, "gbm_device_destroy"));
		if (gbm_destroy)
			gbm_destroy(dev);
		dlclose(lib);
		close(fd);
	}
#endif
	return EGL_NO_DISPLAY;
}

EGLSurface GLContextEGL::CreatePlatformSurface(EGLConfig config, void* win, Error* error)
{
	EGLSurface surface = TryCreatePlatformSurface(config, win, error);
	if (!surface)
		surface = CreateFallbackSurface(config, win, error);
	return surface;
}

EGLDisplay GLContextEGL::TryGetPlatformDisplay(EGLenum platform, const char* platform_ext)
{
	const char* extensions_str = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
	if (!extensions_str)
	{
		Console.WriteLn("No extensions supported.");
		return EGL_NO_DISPLAY;
	}

	EGLDisplay dpy = EGL_NO_DISPLAY;
	if (platform_ext && std::strstr(extensions_str, platform_ext))
	{
		Console.WriteLnFmt("Using EGL platform {}.", platform_ext);

		PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display_ext =
			(PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
		if (get_platform_display_ext)
		{
			dpy = get_platform_display_ext(platform, m_wi.display_connection, nullptr);
			m_use_ext_platform_base = (dpy != EGL_NO_DISPLAY);
			if (!m_use_ext_platform_base)
			{
				const EGLint err = eglGetError();
				Console.ErrorFmt("eglGetPlatformDisplayEXT() failed: {} (0x{:X})", err, err);
			}
		}
		else
		{
			Console.Warning("eglGetPlatformDisplayEXT() was not found");
		}
	}
	else
	{
		Console.WarningFmt("{} is not supported.", platform_ext);
	}

	return dpy;
}

EGLSurface GLContextEGL::TryCreatePlatformSurface(EGLConfig config, void* win, Error* error)
{
	EGLSurface surface = EGL_NO_SURFACE;
	if (m_use_ext_platform_base)
	{
		PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC create_platform_window_surface_ext =
			(PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC)eglGetProcAddress("eglCreatePlatformWindowSurfaceEXT");
		if (create_platform_window_surface_ext)
		{
			surface = create_platform_window_surface_ext(m_display, config, win, nullptr);
			if (surface == EGL_NO_SURFACE)
			{
				const EGLint err = eglGetError();
				Error::SetStringFmt(error, "eglCreatePlatformWindowSurfaceEXT() failed: {} (0x{:X})", err, err);
			}
		}
		else
		{
			Console.Error("eglCreatePlatformWindowSurfaceEXT() not found");
		}
	}

	return surface;
}

EGLDisplay GLContextEGL::GetFallbackDisplay(Error* error)
{
	Console.Warning("Using fallback eglGetDisplay() path.");

	EGLDisplay dpy = eglGetDisplay(m_wi.display_connection);
	if (dpy == EGL_NO_DISPLAY)
	{
		const EGLint err = eglGetError();
		Error::SetStringFmt(error, "eglGetDisplay() failed: {} (0x{:X})", err, err);
	}

	return dpy;
}

EGLSurface GLContextEGL::CreateFallbackSurface(EGLConfig config, void* win, Error* error)
{
	Console.Warning("Using fallback eglCreateWindowSurface() path.");

	EGLSurface surface = eglCreateWindowSurface(m_display, config, (EGLNativeWindowType)win, nullptr);
	if (surface == EGL_NO_SURFACE)
	{
		const EGLint err = eglGetError();
		Error::SetStringFmt(error, "eglCreateWindowSurface() failed: {} (0x{:X})", err, err);
	}

	return surface;
}

void* GLContextEGL::GetProcAddress(const char* name)
{
	return reinterpret_cast<void*>(eglGetProcAddress(name));
}

bool GLContextEGL::ChangeSurface(const WindowInfo& new_wi)
{
	const bool was_current = (eglGetCurrentContext() == m_context);
	if (was_current)
		eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

	if (m_surface != EGL_NO_SURFACE)
	{
		eglDestroySurface(m_display, m_surface);
		m_surface = EGL_NO_SURFACE;
	}

	m_wi = new_wi;
	if (!CreateSurface())
		return false;

	if (was_current && !eglMakeCurrent(m_display, m_surface, m_surface, m_context))
	{
		Console.Error("Failed to make context current again after surface change");
		return false;
	}

	return true;
}

void GLContextEGL::ResizeSurface(u32 new_surface_width /*= 0*/, u32 new_surface_height /*= 0*/)
{
	if (new_surface_width == 0 && new_surface_height == 0)
	{
		EGLint surface_width, surface_height;
		if (eglQuerySurface(m_display, m_surface, EGL_WIDTH, &surface_width) &&
			eglQuerySurface(m_display, m_surface, EGL_HEIGHT, &surface_height))
		{
			m_wi.surface_width = static_cast<u32>(surface_width);
			m_wi.surface_height = static_cast<u32>(surface_height);
			return;
		}
		else
		{
			Console.ErrorFmt("eglQuerySurface() failed: 0x{:x}", eglGetError());
		}
	}

	m_wi.surface_width = new_surface_width;
	m_wi.surface_height = new_surface_height;
}

bool GLContextEGL::SwapBuffers()
{
	return eglSwapBuffers(m_display, m_surface);
}

bool GLContextEGL::IsCurrent()
{
	return m_context && eglGetCurrentContext() == m_context;
}

bool GLContextEGL::MakeCurrent()
{
	if (!eglMakeCurrent(m_display, m_surface, m_surface, m_context))
	{
		Console.ErrorFmt("eglMakeCurrent() failed: 0x{:x}", eglGetError());
		return false;
	}

	return true;
}

bool GLContextEGL::DoneCurrent()
{
	return eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

bool GLContextEGL::SupportsNegativeSwapInterval() const
{
	return m_supports_negative_swap_interval;
}

bool GLContextEGL::SetSwapInterval(s32 interval)
{
	return eglSwapInterval(m_display, interval);
}

std::unique_ptr<GLContext> GLContextEGL::CreateSharedContext(const WindowInfo& wi, Error* error)
{
	std::unique_ptr<GLContextEGL> context = std::make_unique<GLContextEGL>(wi);
	context->m_display = m_display;

	if (!context->CreateContextAndSurface(m_version, m_context, false))
	{
		Error::SetStringView(error, "Failed to create context/surface");
		return nullptr;
	}

	return context;
}

bool GLContextEGL::CreateSurface()
{
	if (m_wi.type == WindowInfo::Type::Surfaceless)
	{
		if (GLAD_EGL_KHR_surfaceless_context)
			return true;
		else
			return CreatePBufferSurface();
	}

	Error error;
	m_surface = CreatePlatformSurface(m_config, m_wi.window_handle, &error);
	if (m_surface == EGL_NO_SURFACE)
	{
		Console.ErrorFmt("Failed to create platform surface: {}", error.GetDescription());
		return false;
	}

	// Some implementations may require the size to be queried at runtime.
	EGLint surface_width, surface_height;
	if (eglQuerySurface(m_display, m_surface, EGL_WIDTH, &surface_width) &&
		eglQuerySurface(m_display, m_surface, EGL_HEIGHT, &surface_height))
	{
		m_wi.surface_width = static_cast<u32>(surface_width);
		m_wi.surface_height = static_cast<u32>(surface_height);
	}
	else
	{
		Console.ErrorFmt("eglQuerySurface() failed: 0x{:x}", eglGetError());
	}

	return true;
}

bool GLContextEGL::CreatePBufferSurface()
{
	const u32 width = std::max<u32>(m_wi.surface_width, 1);
	const u32 height = std::max<u32>(m_wi.surface_height, 1);

	// TODO: Format
	EGLint attrib_list[] = {
		EGL_WIDTH,
		static_cast<EGLint>(width),
		EGL_HEIGHT,
		static_cast<EGLint>(height),
		EGL_NONE,
	};

	m_surface = eglCreatePbufferSurface(m_display, m_config, attrib_list);
	if (!m_surface)
	{
		Console.Error("eglCreatePbufferSurface() failed: 0x{:x}", eglGetError());
		return false;
	}

	DevCon.WriteLnFmt("Created {}x{} pbuffer surface", width, height);
	return true;
}

bool GLContextEGL::CheckConfigSurfaceFormat(EGLConfig config)
{
	int red_size, green_size, blue_size, alpha_size;
	if (!eglGetConfigAttrib(m_display, config, EGL_RED_SIZE, &red_size) ||
		!eglGetConfigAttrib(m_display, config, EGL_GREEN_SIZE, &green_size) ||
		!eglGetConfigAttrib(m_display, config, EGL_BLUE_SIZE, &blue_size) ||
		!eglGetConfigAttrib(m_display, config, EGL_ALPHA_SIZE, &alpha_size))
	{
		return false;
	}

	return (red_size == 8 && green_size == 8 && blue_size == 8);
}

void GLContextEGL::DestroyContext()
{
	if (eglGetCurrentContext() == m_context)
		eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

	if (m_context != EGL_NO_CONTEXT)
	{
		eglDestroyContext(m_display, m_context);
		m_context = EGL_NO_CONTEXT;
	}
}

void GLContextEGL::DestroySurface()
{
	if (eglGetCurrentSurface(EGL_DRAW) == m_surface)
		eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

	if (m_surface != EGL_NO_SURFACE)
	{
		eglDestroySurface(m_display, m_surface);
		m_surface = EGL_NO_SURFACE;
	}
}

bool GLContextEGL::CreateContext(const Version& version, EGLContext share_context)
{
	DevCon.WriteLnFmt("Trying {} version {}.{}", version.is_gles ? "GLES" : "GL",
		version.major_version, version.minor_version);

	const EGLint renderable_type = version.is_gles ? EGL_OPENGL_ES3_BIT : EGL_OPENGL_BIT;

	// For surfaceless contexts we omit EGL_SURFACE_TYPE so the config
	// search is not restricted — V3D via GBM only exposes desktop-GL
	// configs with EGL_WINDOW_BIT, but surfaceless contexts work with any
	// config.  For windowed contexts keep EGL_WINDOW_BIT.
	const int surface_attribs_surfaceless[] = {
		EGL_RENDERABLE_TYPE, renderable_type,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
		EGL_NONE, 0};
	const int surface_attribs_window[] = {
		EGL_RENDERABLE_TYPE, renderable_type,
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
		EGL_NONE, 0};
	const int* surface_attribs =
		(m_wi.type == WindowInfo::Type::Surfaceless) ? surface_attribs_surfaceless : surface_attribs_window;

	EGLint num_configs;
	if (!eglChooseConfig(m_display, surface_attribs, nullptr, 0, &num_configs) || num_configs == 0)
	{
		// Expected on GLES-only systems when trying desktop GL (or vice-versa).
		DevCon.WriteLnFmt("eglChooseConfig() found no configs (0x{:x})", eglGetError());
		return false;
	}

	std::vector<EGLConfig> configs(static_cast<u32>(num_configs));
	if (!eglChooseConfig(m_display, surface_attribs, configs.data(), num_configs, &num_configs))
	{
		Console.ErrorFmt("eglChooseConfig() failed: 0x{:x}", eglGetError());
		return false;
	}
	configs.resize(static_cast<u32>(num_configs));

	std::optional<EGLConfig> config;
	for (EGLConfig check_config : configs)
	{
		if (CheckConfigSurfaceFormat(check_config))
		{
			config = check_config;
			break;
		}
	}

	if (!config.has_value())
	{
		Console.Warning("No EGL configs matched exactly, using first.");
		config = configs.front();
	}

	EGLContext context;
	if (version.is_gles)
	{
		// OpenGL ES: no profile mask needed, just major/minor version.
		// DIAGNOSTIC (V3D/RPi5 texture flicker): request a DEBUG context so Mesa/V3D
		// actually emits GL_KHR_debug messages to our callback — without the debug bit
		// the driver stays silent even with the callback installed. REMOVE at cleanup.
		const int attribs[] = {
			EGL_CONTEXT_MAJOR_VERSION, version.major_version,
			EGL_CONTEXT_MINOR_VERSION, version.minor_version,
			EGL_CONTEXT_OPENGL_DEBUG, EGL_TRUE,
			EGL_NONE, 0};

		if (!eglBindAPI(EGL_OPENGL_ES_API))
		{
			Console.ErrorFmt("eglBindAPI(EGL_OPENGL_ES_API) failed: 0x{:x}", eglGetError());
			return false;
		}

		context = eglCreateContext(m_display, config.value(), share_context, attribs);
		if (!context)
		{
			// DIAGNOSTIC fallback: some drivers reject the debug bit — retry without it.
			Console.WarningFmt("eglCreateContext() with debug bit failed (0x{:x}); retrying without.", eglGetError());
			const int attribs_nodebug[] = {
				EGL_CONTEXT_MAJOR_VERSION, version.major_version,
				EGL_CONTEXT_MINOR_VERSION, version.minor_version,
				EGL_NONE, 0};
			context = eglCreateContext(m_display, config.value(), share_context, attribs_nodebug);
		}
	}
	else
	{
		// Desktop OpenGL: request core profile (required on Mesa V3D / RPi5).
		const int attribs[] = {
			EGL_CONTEXT_MAJOR_VERSION, version.major_version,
			EGL_CONTEXT_MINOR_VERSION, version.minor_version,
			EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR,
			EGL_NONE, 0};

		if (!eglBindAPI(EGL_OPENGL_API))
		{
			DevCon.WriteLnFmt("eglBindAPI(EGL_OPENGL_API) not supported (0x{:x})", eglGetError());
			return false;
		}

		context = eglCreateContext(m_display, config.value(), share_context, attribs);
	}

	m_context = context;
	if (!m_context)
	{
		Console.ErrorFmt("eglCreateContext() failed: 0x{:x}", eglGetError());
		return false;
	}

	Console.WriteLnFmt("Got {} version {}.{}", version.is_gles ? "GLES" : "GL",
		version.major_version, version.minor_version);
	m_is_gles = version.is_gles;

	EGLint min_swap_interval, max_swap_interval;
	m_supports_negative_swap_interval = false;
	if (eglGetConfigAttrib(m_display, config.value(), EGL_MIN_SWAP_INTERVAL, &min_swap_interval) &&
		eglGetConfigAttrib(m_display, config.value(), EGL_MAX_SWAP_INTERVAL, &max_swap_interval))
	{
		DEV_LOG("EGL_MIN_SWAP_INTERVAL = {}", min_swap_interval);
		DEV_LOG("EGL_MAX_SWAP_INTERVAL = {}", max_swap_interval);
		m_supports_negative_swap_interval = (min_swap_interval <= -1);
	}

	INFO_LOG("Negative swap interval/tear-control is {}supported", m_supports_negative_swap_interval ? "" : "NOT ");

	m_config = config.value();
	m_version = version;
	return true;
}

bool GLContextEGL::CreateContextAndSurface(const Version& version, EGLContext share_context, bool make_current)
{
	if (!CreateContext(version, share_context))
		return false;

	if (!CreateSurface())
	{
		Console.Error("Failed to create surface for context");
		eglDestroyContext(m_display, m_context);
		m_context = EGL_NO_CONTEXT;
		return false;
	}

	if (make_current && !eglMakeCurrent(m_display, m_surface, m_surface, m_context))
	{
		Console.ErrorFmt("eglMakeCurrent() failed: 0x{:x}", eglGetError());
		if (m_surface != EGL_NO_SURFACE)
		{
			eglDestroySurface(m_display, m_surface);
			m_surface = EGL_NO_SURFACE;
		}
		eglDestroyContext(m_display, m_context);
		m_context = EGL_NO_CONTEXT;
		return false;
	}

	return true;
}
