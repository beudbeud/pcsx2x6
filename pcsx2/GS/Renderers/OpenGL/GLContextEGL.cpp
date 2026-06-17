// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/OpenGL/GLContextEGL.h"

#include "glad/gl.h" // gl* entry points for the linear dmabuf render target

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
	DestroyLinearDmaBufTexture(); // before the context/GBM teardown below
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

	// Headless/libretro path: prefer the GBM platform (required for the KMS/DRM
	// zero-copy dmabuf path on Recalbox), but fall back to EGL_MESA_platform_surfaceless
	// when GBM yields no usable current context. That is what happens on a normal
	// desktop: a compositor owns the DRM card node, so the GBM display initialises and
	// contexts are created, yet eglMakeCurrent() fails for every version — leaving the
	// device-create dead unless we retry on the surfaceless platform.
	if (m_wi.type == WindowInfo::Type::Surfaceless)
	{
		const EGLDisplay gbm_dpy = TryGBMPlatformDisplay(error);
		if (gbm_dpy != EGL_NO_DISPLAY)
		{
			if (TryDisplay(gbm_dpy, versions_to_try, error))
				return true;

			Console.Warning("GBM platform display produced no usable GL context; "
							"falling back to EGL_MESA_platform_surfaceless.");
			ReleaseGBMDisplay();
		}

		// These offscreen platforms have no window configs, so config selection
		// must request EGL_PBUFFER_BIT and bind a pbuffer (see CreateContext()/
		// CreateSurface()), not the EGL_WINDOW_BIT default.
		m_surfaceless_platform = true;

		// 2. EGL device platform. This is the path that works on an NVIDIA desktop
		//    (GBM/surfaceless route through libEGL_mesa and fail eglMakeCurrent on
		//    the real NVIDIA GPU); eglQueryDevicesEXT exposes the NVIDIA EGL device
		//    directly. Harmless elsewhere — Mesa-HW desktops already succeed via GBM.
		if (TryDevicePlatformDisplays(versions_to_try, error))
			return true;

		// 3. Surfaceless platform (last resort).
		EGLDisplay sfl_dpy = TryGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, "EGL_MESA_platform_surfaceless");
		if (sfl_dpy == EGL_NO_DISPLAY)
			sfl_dpy = GetFallbackDisplay(error);
		if (sfl_dpy != EGL_NO_DISPLAY && TryDisplay(sfl_dpy, versions_to_try, error))
			return true;

		Error::SetStringView(error, "Failed to create any context versions");
		return false;
	}

	// Windowed path (includes the X11/Wayland subclass overrides of GetPlatformDisplay()).
	const EGLDisplay dpy = GetPlatformDisplay(error);
	if (dpy == EGL_NO_DISPLAY)
		return false;

	return TryDisplay(dpy, versions_to_try, error);
}

// Initialise EGL on the given display and try to create one of the requested contexts.
// On failure the display is terminated and m_display reset, so the caller can try another.
bool GLContextEGL::TryDisplay(EGLDisplay dpy, std::span<const Version> versions_to_try, Error* error)
{
	m_display = dpy;

	int egl_major, egl_minor;
	if (!eglInitialize(m_display, &egl_major, &egl_minor))
	{
		const int gerror = static_cast<int>(eglGetError());
		Error::SetStringFmt(error, "eglInitialize() failed: {} (0x{:X})", gerror, gerror);
		m_display = EGL_NO_DISPLAY;
		return false;
	}

	Console.WriteLnFmt("eglInitialize() version: {}.{}", egl_major, egl_minor);

	// Log supported client APIs — if "OpenGL" is absent, only GLES is available.
	const char* client_apis = eglQueryString(m_display, EGL_CLIENT_APIS);
	Console.WriteLnFmt("EGL client APIs: {}", client_apis ? client_apis : "(null)");

	// Re-initialize EGL/GLAD.
	if (!LoadGLADEGL(m_display, error))
	{
		eglTerminate(m_display);
		m_display = EGL_NO_DISPLAY;
		return false;
	}

	// Zero-copy HW render feasibility probe (one driver = V3D, so this display's support is
	// representative of the frontend's). dmabuf export of the GS render target + import on
	// the frontend context is the route that avoids both the dual-EGLDisplay/KMS conflict
	// and the mid-frame EE<->GS sync deadlock. Export side: EGL_MESA_image_dma_buf_export;
	// import side: EGL_EXT_image_dma_buf_import (+ GL_OES_EGL_image, checked once GL loads).
	{
		const char* dpy_ext = eglQueryString(m_display, EGL_EXTENSIONS);
		const bool has_export = dpy_ext && std::strstr(dpy_ext, "EGL_MESA_image_dma_buf_export");
		const bool has_import = dpy_ext && std::strstr(dpy_ext, "EGL_EXT_image_dma_buf_import");
		Console.WriteLnFmt("dmabuf probe: EGL_MESA_image_dma_buf_export={}, EGL_EXT_image_dma_buf_import={}",
			has_export ? "yes" : "NO", has_import ? "yes" : "NO");
	}

	if (!GLAD_EGL_KHR_surfaceless_context)
		Console.Warning("EGL implementation does not support surfaceless contexts, emulating with pbuffers");

	for (const Version& cv : versions_to_try)
	{
		if (CreateContextAndSurface(cv, nullptr, true))
			return true;
	}

	Error::SetStringView(error, "Failed to create any context versions");
	eglTerminate(m_display);
	m_display = EGL_NO_DISPLAY;
	return false;
}

// Tear down the GBM EGL device acquired by TryGBMPlatformDisplay() so the surfaceless
// fallback starts clean (and the dmabuf-linear path doesn't think GBM is still available).
void GLContextEGL::ReleaseGBMDisplay()
{
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
	m_gbm_device = nullptr;
	m_gbm_lib = nullptr;
	m_gbm_fd = -1;
#endif
}

bool GLContextEGL::ExportTextureDMABUF(u32 texture_id, DmaBufFrame* out)
{
	// Zero-copy HW render: wrap the GL texture in an EGLImage and export it as a dmabuf the
	// libretro frontend context can import. Called on the GS thread (its context is current).
	//
	// glad loaded the GS EGL via dlsym on libEGL.so, which resolves CORE symbols but NOT the
	// KHR/MESA extension entry points (on Mesa those are only reachable through
	// eglGetProcAddress) — so glad's eglCreateImageKHR/etc pointers are null even though the
	// extension flags are set. Load them ourselves via eglGetProcAddress (same pattern as the
	// eglGetPlatformDisplayEXT lookup above). Cached: resolved once.
	static const auto s_eglCreateImageKHR =
		reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
	static const auto s_eglDestroyImageKHR =
		reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
	static const auto s_eglExportDMABUFImageQueryMESA =
		reinterpret_cast<PFNEGLEXPORTDMABUFIMAGEQUERYMESAPROC>(eglGetProcAddress("eglExportDMABUFImageQueryMESA"));
	static const auto s_eglExportDMABUFImageMESA =
		reinterpret_cast<PFNEGLEXPORTDMABUFIMAGEMESAPROC>(eglGetProcAddress("eglExportDMABUFImageMESA"));

	if (!s_eglCreateImageKHR || !s_eglDestroyImageKHR || !s_eglExportDMABUFImageQueryMESA ||
		!s_eglExportDMABUFImageMESA)
	{
		Console.WarningFmt("dmabuf export: eglGetProcAddress missing (create={}, destroy={}, query={}, export={}).",
			s_eglCreateImageKHR != nullptr, s_eglDestroyImageKHR != nullptr,
			s_eglExportDMABUFImageQueryMESA != nullptr, s_eglExportDMABUFImageMESA != nullptr);
		return false;
	}

	const EGLImageKHR image = s_eglCreateImageKHR(m_display, eglGetCurrentContext(), EGL_GL_TEXTURE_2D_KHR,
		reinterpret_cast<EGLClientBuffer>(static_cast<uintptr_t>(texture_id)), nullptr);
	if (image == EGL_NO_IMAGE_KHR)
	{
		Console.WarningFmt("dmabuf export: eglCreateImageKHR failed (0x{:x})", static_cast<int>(eglGetError()));
		return false;
	}

	int fourcc = 0, num_planes = 0;
	EGLuint64KHR modifier = 0;
	int fd = -1;
	EGLint stride = 0, offset = 0;
	const bool ok = s_eglExportDMABUFImageQueryMESA(m_display, image, &fourcc, &num_planes, &modifier) &&
		num_planes == 1 && s_eglExportDMABUFImageMESA(m_display, image, &fd, &stride, &offset) && fd >= 0;

	s_eglDestroyImageKHR(m_display, image); // the dmabuf fd keeps the underlying buffer alive

	if (!ok)
	{
		Console.WarningFmt("dmabuf export: query/export failed (planes={}, err 0x{:x})", num_planes,
			static_cast<int>(eglGetError()));
		if (fd >= 0)
			close(fd);
		return false;
	}

	out->fd = fd;
	out->stride = static_cast<u32>(stride);
	out->offset = static_cast<u32>(offset);
	out->fourcc = static_cast<u32>(fourcc);
	out->modifier = static_cast<u64>(modifier);
	return true;
}

#ifndef _WIN32
// GBM / EGL dmabuf-import bits not guaranteed in the EGL headers we link against.
#ifndef EGL_LINUX_DMA_BUF_EXT
#define EGL_LINUX_DMA_BUF_EXT 0x3270
#define EGL_LINUX_DRM_FOURCC_EXT 0x3271
#define EGL_DMA_BUF_PLANE0_FD_EXT 0x3272
#define EGL_DMA_BUF_PLANE0_OFFSET_EXT 0x3273
#define EGL_DMA_BUF_PLANE0_PITCH_EXT 0x3274
#endif
static constexpr u32 PCSX2_GBM_FORMAT_ABGR8888 = 0x34324241; // 'AB24' == GL_RGBA8 byte order
static constexpr u32 PCSX2_GBM_BO_USE_RENDERING = (1u << 2);
static constexpr u32 PCSX2_GBM_BO_USE_LINEAR = (1u << 4);
typedef void* (*PFN_gbm_bo_create)(void*, u32, u32, u32, u32);
typedef int (*PFN_gbm_bo_get_fd)(void*);
typedef u32 (*PFN_gbm_bo_get_stride)(void*);
typedef void (*PFN_gbm_bo_destroy)(void*);
typedef void (*PFN_glEGLImageTargetTexture2DOES_local)(GLenum, void*); // GL_APIENTRY is empty on Linux
#endif

bool GLContextEGL::CreateLinearDmaBufTexture(u32 width, u32 height, DmaBufFrame* out, u32* out_texture)
{
#ifndef _WIN32
	if (!m_gbm_device || !m_gbm_lib)
	{
		Console.Warning("dmabuf linear: no GBM device (export needs the GBM platform).");
		return false;
	}

	auto gbm_bo_create = reinterpret_cast<PFN_gbm_bo_create>(dlsym(m_gbm_lib, "gbm_bo_create"));
	auto gbm_bo_get_fd = reinterpret_cast<PFN_gbm_bo_get_fd>(dlsym(m_gbm_lib, "gbm_bo_get_fd"));
	auto gbm_bo_get_stride = reinterpret_cast<PFN_gbm_bo_get_stride>(dlsym(m_gbm_lib, "gbm_bo_get_stride"));
	if (!gbm_bo_create || !gbm_bo_get_fd || !gbm_bo_get_stride)
	{
		Console.Warning("dmabuf linear: missing gbm_bo_* symbols.");
		return false;
	}

	static const auto s_eglCreateImageKHR =
		reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
	static const auto s_glEGLImageTargetTexture2DOES =
		reinterpret_cast<PFN_glEGLImageTargetTexture2DOES_local>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
	if (!s_eglCreateImageKHR || !s_glEGLImageTargetTexture2DOES)
	{
		Console.Warning("dmabuf linear: eglGetProcAddress missing image entry points.");
		return false;
	}

	void* bo = gbm_bo_create(m_gbm_device, width, height, PCSX2_GBM_FORMAT_ABGR8888,
		PCSX2_GBM_BO_USE_RENDERING | PCSX2_GBM_BO_USE_LINEAR);
	if (!bo)
	{
		Console.Warning("dmabuf linear: gbm_bo_create(LINEAR|RENDERING) failed (driver may not support a "
						"linear render target).");
		return false;
	}

	const int fd = gbm_bo_get_fd(bo);
	const u32 stride = gbm_bo_get_stride(bo);
	if (fd < 0)
	{
		Console.Warning("dmabuf linear: gbm_bo_get_fd failed.");
		auto destroy = reinterpret_cast<PFN_gbm_bo_destroy>(dlsym(m_gbm_lib, "gbm_bo_destroy"));
		if (destroy)
			destroy(bo);
		return false;
	}

	// Import the (linear) dmabuf as an EGLImage -> GL texture in this context. No modifier attribs:
	// the buffer is linear, so the default layout is exactly right (and avoids needing the
	// import-modifiers extension for the simple case).
	const EGLint attribs[] = {
		EGL_WIDTH, static_cast<EGLint>(width),
		EGL_HEIGHT, static_cast<EGLint>(height),
		EGL_LINUX_DRM_FOURCC_EXT, static_cast<EGLint>(PCSX2_GBM_FORMAT_ABGR8888),
		EGL_DMA_BUF_PLANE0_FD_EXT, fd,
		EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
		EGL_DMA_BUF_PLANE0_PITCH_EXT, static_cast<EGLint>(stride),
		EGL_NONE};
	const EGLImageKHR image = s_eglCreateImageKHR(m_display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
		static_cast<EGLClientBuffer>(nullptr), attribs);
	if (image == EGL_NO_IMAGE_KHR)
	{
		Console.WarningFmt("dmabuf linear: eglCreateImageKHR failed (0x{:x}).", static_cast<int>(eglGetError()));
		close(fd);
		auto destroy = reinterpret_cast<PFN_gbm_bo_destroy>(dlsym(m_gbm_lib, "gbm_bo_destroy"));
		if (destroy)
			destroy(bo);
		return false;
	}

	GLuint tex = 0;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	s_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_2D, 0); // classic binding only; GS uses DSA, so GLState stays consistent

	m_lin_bo = bo;
	m_lin_image = image;
	m_lin_tex = tex;
	m_lin_fd = fd;

	out->fd = fd;
	out->width = width;
	out->height = height;
	out->stride = stride;
	out->offset = 0;
	out->fourcc = PCSX2_GBM_FORMAT_ABGR8888;
	out->modifier = 0; // DRM_FORMAT_MOD_LINEAR
	*out_texture = tex;
	return true;
#else
	return false;
#endif
}

void GLContextEGL::DestroyLinearDmaBufTexture()
{
#ifndef _WIN32
	if (m_lin_tex)
	{
		glDeleteTextures(1, &m_lin_tex);
		m_lin_tex = 0;
	}
	if (m_lin_image)
	{
		static const auto s_eglDestroyImageKHR =
			reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
		if (s_eglDestroyImageKHR)
			s_eglDestroyImageKHR(m_display, m_lin_image);
		m_lin_image = nullptr;
	}
	if (m_lin_bo && m_gbm_lib)
	{
		auto destroy = reinterpret_cast<PFN_gbm_bo_destroy>(dlsym(m_gbm_lib, "gbm_bo_destroy"));
		if (destroy)
			destroy(m_lin_bo);
		m_lin_bo = nullptr;
	}
	if (m_lin_fd >= 0)
	{
		close(m_lin_fd);
		m_lin_fd = -1;
	}
#endif
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

// Enumerate EGL devices (EGL_EXT_platform_device) and try each one until a usable
// GL context is created. This is the headless path that works on NVIDIA's EGL,
// where the Mesa GBM/surfaceless platforms can't make a context current.
bool GLContextEGL::TryDevicePlatformDisplays(std::span<const Version> versions_to_try, Error* error)
{
	const char* client_ext = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
	if (!client_ext || !std::strstr(client_ext, "EGL_EXT_platform_device") ||
		!std::strstr(client_ext, "EGL_EXT_device_base"))
	{
		// EGL_EXT_device_base (or its older EGL_EXT_device_enumeration) is what
		// provides eglQueryDevicesEXT; without it we can't enumerate devices.
		if (!client_ext || !std::strstr(client_ext, "EGL_EXT_platform_device"))
			return false;
	}

	const auto query_devices =
		reinterpret_cast<PFNEGLQUERYDEVICESEXTPROC>(eglGetProcAddress("eglQueryDevicesEXT"));
	const auto get_platform_display_ext =
		reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
	if (!query_devices || !get_platform_display_ext)
		return false;

	EGLDeviceEXT devices[16];
	EGLint num_devices = 0;
	if (!query_devices(static_cast<EGLint>(std::size(devices)), devices, &num_devices) || num_devices <= 0)
		return false;

	for (EGLint i = 0; i < num_devices; i++)
	{
		const EGLDisplay dpy = get_platform_display_ext(EGL_PLATFORM_DEVICE_EXT, devices[i], nullptr);
		if (dpy == EGL_NO_DISPLAY)
			continue;

		Console.WriteLnFmt("Trying EGL device platform (device {} of {}).", i + 1, num_devices);
		if (TryDisplay(dpy, versions_to_try, error))
			return true;
	}

	return false;
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
		// On the EGL_MESA_platform_surfaceless display, several desktop Mesa drivers
		// reject eglMakeCurrent() with EGL_NO_SURFACE even though they advertise
		// KHR_surfaceless_context (it returns EGL_FALSE with EGL_SUCCESS). A real
		// pbuffer works (the platform's configs carry EGL_PBUFFER_BIT). GBM/V3D
		// keeps the genuine surfaceless path (NO_SURFACE), which it supports.
		if (m_surfaceless_platform)
			return CreatePBufferSurface();
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

	// Surface-type filtering for surfaceless contexts is platform-dependent:
	//  - GBM (V3D/Recalbox) only exposes desktop-GL/GLES configs flagged
	//    EGL_WINDOW_BIT, so we must keep the eglChooseConfig default (which IS
	//    EGL_WINDOW_BIT) to find them; the context is made current with NO_SURFACE.
	//  - EGL_MESA_platform_surfaceless exposes no window configs but does expose
	//    EGL_PBUFFER_BIT ones, and a real pbuffer is needed there for
	//    eglMakeCurrent() to succeed on desktop Mesa (see CreateSurface()).
	const int surface_attribs_surfaceless[] = {
		EGL_RENDERABLE_TYPE, renderable_type,
		EGL_SURFACE_TYPE, m_surfaceless_platform ? EGL_PBUFFER_BIT : EGL_WINDOW_BIT,
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
		const int attribs[] = {
			EGL_CONTEXT_MAJOR_VERSION, version.major_version,
			EGL_CONTEXT_MINOR_VERSION, version.minor_version,
			EGL_NONE, 0};

		if (!eglBindAPI(EGL_OPENGL_ES_API))
		{
			Console.ErrorFmt("eglBindAPI(EGL_OPENGL_ES_API) failed: 0x{:x}", eglGetError());
			return false;
		}

		context = eglCreateContext(m_display, config.value(), share_context, attribs);
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
