// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/OpenGL/GLContextGLX.h"

#include "common/Console.h"
#include "common/Error.h"

#include <cstring>
#include <dlfcn.h>

#include <X11/Xlib.h>
#include <GL/glx.h>
#include <GL/glxext.h>

namespace
{
	// libX11 / libGL entry points, loaded via dlopen so there is no hard link dependency
	// (a KMS/headless system without an X server simply fails to open the display).
	struct GLXLib
	{
		void* x11 = nullptr;
		void* gl = nullptr;

		Display* (*XOpenDisplay)(const char*) = nullptr;
		int (*XCloseDisplay)(Display*) = nullptr;
		int (*XDefaultScreen)(Display*) = nullptr;
		int (*XSync)(Display*, Bool) = nullptr;
		int (*XInitThreads)() = nullptr;
		XErrorHandler (*XSetErrorHandler)(XErrorHandler) = nullptr;

		__GLXextFuncPtr (*glXGetProcAddressARB)(const GLubyte*) = nullptr;
		GLXFBConfig* (*glXChooseFBConfig)(Display*, int, const int*, int*) = nullptr;
		GLXContext (*glXCreateNewContext)(Display*, GLXFBConfig, int, GLXContext, Bool) = nullptr;
		GLXPbuffer (*glXCreatePbuffer)(Display*, GLXFBConfig, const int*) = nullptr;
		Bool (*glXMakeContextCurrent)(Display*, GLXDrawable, GLXDrawable, GLXContext) = nullptr;
		void (*glXDestroyContext)(Display*, GLXContext) = nullptr;
		void (*glXDestroyPbuffer)(Display*, GLXPbuffer) = nullptr;
		GLXContext (*glXGetCurrentContext)() = nullptr;
		int (*XFree)(void*) = nullptr;

		bool loaded = false;
	};

	GLXLib s_glx;

	template <typename T>
	bool LoadSym(void* lib, T& fn, const char* name)
	{
		fn = reinterpret_cast<T>(dlsym(lib, name));
		return fn != nullptr;
	}

	bool EnsureGLXLib()
	{
		if (s_glx.loaded)
			return true;

		s_glx.x11 = dlopen("libX11.so.6", RTLD_NOW | RTLD_GLOBAL);
		s_glx.gl = dlopen("libGL.so.1", RTLD_NOW | RTLD_GLOBAL);
		if (!s_glx.x11 || !s_glx.gl)
			return false;

		bool ok = true;
		ok &= LoadSym(s_glx.x11, s_glx.XOpenDisplay, "XOpenDisplay");
		ok &= LoadSym(s_glx.x11, s_glx.XCloseDisplay, "XCloseDisplay");
		ok &= LoadSym(s_glx.x11, s_glx.XDefaultScreen, "XDefaultScreen");
		ok &= LoadSym(s_glx.x11, s_glx.XSync, "XSync");
		ok &= LoadSym(s_glx.x11, s_glx.XSetErrorHandler, "XSetErrorHandler");
		LoadSym(s_glx.x11, s_glx.XInitThreads, "XInitThreads"); // optional
		LoadSym(s_glx.x11, s_glx.XFree, "XFree"); // optional

		ok &= LoadSym(s_glx.gl, s_glx.glXGetProcAddressARB, "glXGetProcAddressARB");
		ok &= LoadSym(s_glx.gl, s_glx.glXChooseFBConfig, "glXChooseFBConfig");
		ok &= LoadSym(s_glx.gl, s_glx.glXCreateNewContext, "glXCreateNewContext");
		ok &= LoadSym(s_glx.gl, s_glx.glXCreatePbuffer, "glXCreatePbuffer");
		ok &= LoadSym(s_glx.gl, s_glx.glXMakeContextCurrent, "glXMakeContextCurrent");
		ok &= LoadSym(s_glx.gl, s_glx.glXDestroyContext, "glXDestroyContext");
		ok &= LoadSym(s_glx.gl, s_glx.glXDestroyPbuffer, "glXDestroyPbuffer");
		ok &= LoadSym(s_glx.gl, s_glx.glXGetCurrentContext, "glXGetCurrentContext");

		s_glx.loaded = ok;
		return ok;
	}

	// Swallow X errors during context creation (an unsupported GL version raises BadMatch/etc.
	// which would otherwise reach the default handler and abort the process).
	int SilentXErrorHandler(Display*, XErrorEvent*)
	{
		return 0;
	}

	// Frontend GLX context to share with (zero-copy HW render). Set by SetShareContext().
	GLXContext s_share_context = nullptr;
	Display* s_share_display = nullptr;
}

void GLContextGLX::SetShareContext(void* glx_context, void* display)
{
	s_share_context = static_cast<GLXContext>(glx_context);
	s_share_display = static_cast<Display*>(display);
}

GLContextGLX::GLContextGLX(const WindowInfo& wi)
	: GLContext(wi)
{
}

GLContextGLX::~GLContextGLX()
{
	Display* dpy = static_cast<Display*>(m_display);
	if (dpy)
	{
		if (s_glx.glXGetCurrentContext && s_glx.glXGetCurrentContext() == static_cast<GLXContext>(m_context))
			s_glx.glXMakeContextCurrent(dpy, None, None, nullptr);
		if (m_pbuffer)
			s_glx.glXDestroyPbuffer(dpy, static_cast<GLXPbuffer>(m_pbuffer));
		if (m_context)
			s_glx.glXDestroyContext(dpy, static_cast<GLXContext>(m_context));
		if (m_owns_display && s_glx.XCloseDisplay)
			s_glx.XCloseDisplay(dpy);
	}
}

std::unique_ptr<GLContext> GLContextGLX::Create(const WindowInfo& wi, std::span<const Version> versions_to_try, Error* error)
{
	// Only the headless/libretro (Surfaceless) case uses this backend; windowed builds use
	// EGLX11/EGLWayland/WGL.
	if (wi.type != WindowInfo::Type::Surfaceless)
		return nullptr;

	if (!EnsureGLXLib())
	{
		Error::SetStringView(error, "libX11/libGL not available for GLX");
		return nullptr;
	}

	std::unique_ptr<GLContextGLX> context = std::make_unique<GLContextGLX>(wi);
	if (!context->Initialize(versions_to_try, error))
		return nullptr;

	return context;
}

bool GLContextGLX::Initialize(std::span<const Version> versions_to_try, Error* error)
{
	if (s_glx.XInitThreads)
		s_glx.XInitThreads();

	// Share with the frontend's GLX context (zero-copy): GLX object sharing requires the same
	// Display connection, so reuse the frontend's captured display in that case.
	Display* dpy = nullptr;
	if (s_share_context && s_share_display)
	{
		dpy = s_share_display;
		m_owns_display = false;
	}
	else
	{
		dpy = s_glx.XOpenDisplay(nullptr); // honours $DISPLAY; null on a KMS/headless system
		m_owns_display = true;
	}
	if (!dpy)
	{
		Error::SetStringView(error, "XOpenDisplay() failed (no X server?)");
		return false;
	}
	m_display = dpy;

	const int screen = s_glx.XDefaultScreen(dpy);

	const int fb_attribs[] = {
		GLX_DRAWABLE_TYPE, GLX_PBUFFER_BIT,
		GLX_RENDER_TYPE, GLX_RGBA_BIT,
		GLX_RED_SIZE, 8,
		GLX_GREEN_SIZE, 8,
		GLX_BLUE_SIZE, 8,
		GLX_ALPHA_SIZE, 8,
		GLX_DOUBLEBUFFER, False,
		None};

	int num_configs = 0;
	GLXFBConfig* configs = s_glx.glXChooseFBConfig(dpy, screen, fb_attribs, &num_configs);
	if (!configs || num_configs <= 0)
	{
		Error::SetStringView(error, "glXChooseFBConfig() found no pbuffer configs");
		return false;
	}
	const GLXFBConfig fb_config = configs[0];
	if (s_glx.XFree)
		s_glx.XFree(configs);
	m_fb_config = fb_config;

	const auto glXCreateContextAttribsARB = reinterpret_cast<PFNGLXCREATECONTEXTATTRIBSARBPROC>(
		s_glx.glXGetProcAddressARB(reinterpret_cast<const GLubyte*>("glXCreateContextAttribsARB")));

	XErrorHandler old_handler = s_glx.XSetErrorHandler(&SilentXErrorHandler);

	GLXContext ctx = nullptr;
	for (const Version& cv : versions_to_try)
	{
		if (cv.is_gles) // GLX path is desktop GL only
			continue;

		if (glXCreateContextAttribsARB)
		{
			const int ctx_attribs[] = {
				GLX_CONTEXT_MAJOR_VERSION_ARB, cv.major_version,
				GLX_CONTEXT_MINOR_VERSION_ARB, cv.minor_version,
				GLX_CONTEXT_PROFILE_MASK_ARB, GLX_CONTEXT_CORE_PROFILE_BIT_ARB,
				None};
			ctx = glXCreateContextAttribsARB(dpy, fb_config, s_share_context, True, ctx_attribs);
		}
		else
		{
			// No ARB_create_context: take whatever the driver gives (no version/profile control).
			ctx = s_glx.glXCreateNewContext(dpy, fb_config, GLX_RGBA_TYPE, s_share_context, True);
		}

		s_glx.XSync(dpy, False);
		if (ctx)
		{
			m_version = cv;
			break;
		}
	}

	s_glx.XSetErrorHandler(old_handler);

	if (!ctx)
	{
		Error::SetStringView(error, "glXCreateContextAttribsARB() failed for all versions");
		return false;
	}
	m_context = ctx;

	const int pbuffer_attribs[] = {
		GLX_PBUFFER_WIDTH, 1,
		GLX_PBUFFER_HEIGHT, 1,
		None};
	const GLXPbuffer pbuffer = s_glx.glXCreatePbuffer(dpy, fb_config, pbuffer_attribs);
	if (!pbuffer)
	{
		Error::SetStringView(error, "glXCreatePbuffer() failed");
		return false;
	}
	m_pbuffer = pbuffer;

	if (!s_glx.glXMakeContextCurrent(dpy, pbuffer, pbuffer, ctx))
	{
		Error::SetStringView(error, "glXMakeContextCurrent() failed");
		return false;
	}

	Console.WriteLnFmt("GLX: created OpenGL {}.{} context ({}).", m_version.major_version,
		m_version.minor_version, s_share_context ? "shared with frontend" : "standalone");
	return true;
}

void* GLContextGLX::GetProcAddress(const char* name)
{
	return reinterpret_cast<void*>(s_glx.glXGetProcAddressARB(reinterpret_cast<const GLubyte*>(name)));
}

bool GLContextGLX::ChangeSurface(const WindowInfo& new_wi)
{
	return true; // offscreen pbuffer; nothing to rebind
}

void GLContextGLX::ResizeSurface(u32 new_surface_width, u32 new_surface_height)
{
	m_wi.surface_width = new_surface_width;
	m_wi.surface_height = new_surface_height;
}

bool GLContextGLX::SwapBuffers()
{
	return true; // present is done by the libretro core (readback or zero-copy blit)
}

bool GLContextGLX::IsCurrent()
{
	return m_context && s_glx.glXGetCurrentContext() == static_cast<GLXContext>(m_context);
}

bool GLContextGLX::MakeCurrent()
{
	Display* dpy = static_cast<Display*>(m_display);
	if (!s_glx.glXMakeContextCurrent(dpy, static_cast<GLXPbuffer>(m_pbuffer),
			static_cast<GLXPbuffer>(m_pbuffer), static_cast<GLXContext>(m_context)))
	{
		Console.Error("GLX: glXMakeContextCurrent() failed");
		return false;
	}
	return true;
}

bool GLContextGLX::DoneCurrent()
{
	return s_glx.glXMakeContextCurrent(static_cast<Display*>(m_display), None, None, nullptr);
}

bool GLContextGLX::SupportsNegativeSwapInterval() const
{
	return false;
}

bool GLContextGLX::SetSwapInterval(s32 interval)
{
	return true; // no on-screen surface to throttle
}

std::unique_ptr<GLContext> GLContextGLX::CreateSharedContext(const WindowInfo& wi, Error* error)
{
	Display* dpy = static_cast<Display*>(m_display);
	std::unique_ptr<GLContextGLX> ctx = std::make_unique<GLContextGLX>(wi);
	ctx->m_display = dpy;
	ctx->m_owns_display = false;
	ctx->m_fb_config = m_fb_config;

	const auto glXCreateContextAttribsARB = reinterpret_cast<PFNGLXCREATECONTEXTATTRIBSARBPROC>(
		s_glx.glXGetProcAddressARB(reinterpret_cast<const GLubyte*>("glXCreateContextAttribsARB")));

	XErrorHandler old_handler = s_glx.XSetErrorHandler(&SilentXErrorHandler);
	GLXContext shared = nullptr;
	if (glXCreateContextAttribsARB)
	{
		const int ctx_attribs[] = {
			GLX_CONTEXT_MAJOR_VERSION_ARB, m_version.major_version,
			GLX_CONTEXT_MINOR_VERSION_ARB, m_version.minor_version,
			GLX_CONTEXT_PROFILE_MASK_ARB, GLX_CONTEXT_CORE_PROFILE_BIT_ARB,
			None};
		shared = glXCreateContextAttribsARB(dpy, static_cast<GLXFBConfig>(m_fb_config),
			static_cast<GLXContext>(m_context), True, ctx_attribs);
	}
	s_glx.XSync(dpy, False);
	s_glx.XSetErrorHandler(old_handler);

	if (!shared)
	{
		Error::SetStringView(error, "GLX: failed to create shared context");
		return nullptr;
	}
	ctx->m_context = shared;

	const int pbuffer_attribs[] = {GLX_PBUFFER_WIDTH, 1, GLX_PBUFFER_HEIGHT, 1, None};
	ctx->m_pbuffer = s_glx.glXCreatePbuffer(dpy, static_cast<GLXFBConfig>(m_fb_config), pbuffer_attribs);
	ctx->m_version = m_version;
	return ctx;
}
