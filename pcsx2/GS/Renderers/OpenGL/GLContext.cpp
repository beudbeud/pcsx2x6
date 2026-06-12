// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/OpenGL/GLContext.h"

#if defined(_WIN32)
#include "GS/Renderers/OpenGL/GLContextWGL.h"
#else // Linux
#ifdef X11_API
#include "GS/Renderers/OpenGL/GLContextEGLX11.h"
#endif
#ifdef WAYLAND_API
#include "GS/Renderers/OpenGL/GLContextEGLWayland.h"
#endif
// Base EGL is always available for surfaceless/KMS contexts (libretro, headless).
#include "GS/Renderers/OpenGL/GLContextEGL.h"
#endif

#include "common/Console.h"
#include "common/Error.h"

#include "glad/gl.h"

GLContext::GLContext(const WindowInfo& wi)
	: m_wi(wi)
{
}

GLContext::~GLContext() = default;

std::unique_ptr<GLContext> GLContext::Create(const WindowInfo& wi, Error* error)
{
	// Desktop GL first (3.3+), then GLES fallback (3.2/3.1) for platforms
	// that only expose GLES (e.g. EGL_MESA_platform_surfaceless on some drivers).
	static constexpr Version vlist[] = {
		{4, 6},
		{4, 5},
		{4, 4},
		{4, 3},
		{4, 2},
		{4, 1},
		{4, 0},
		{3, 3},
		{3, 2, true},
		{3, 1, true},
	};

	std::unique_ptr<GLContext> context;
	Error local_error;
#if defined(_WIN32)
	context = GLContextWGL::Create(wi, vlist, error);
#else // Linux
#if defined(X11_API)
	if (wi.type == WindowInfo::Type::X11)
		context = GLContextEGLX11::Create(wi, vlist, error);
#endif

#if defined(WAYLAND_API)
	if (wi.type == WindowInfo::Type::Wayland)
		context = GLContextEGLWayland::Create(wi, vlist, error);
#endif

	// Fallback: use the base EGL context for surfaceless/KMS builds
	// (libretro, headless). EGL_PLATFORM_SURFACELESS_MESA is tried first.
	if (!context)
		context = GLContextEGL::Create(wi, vlist, error);
#endif

	if (!context)
		return nullptr;

	// NOTE: Not thread-safe. But this is okay, since we're not going to be creating more than one context at a time.
	static GLContext* context_being_created;
	context_being_created = context.get();

	// Load glad — desktop GL or GLES2 depending on which context we got.
	const bool is_gles = context->IsGLES();
	const bool glad_ok = is_gles
		? (gladLoadGLES2([](const char* name) { return reinterpret_cast<GLADapiproc>(context_being_created->GetProcAddress(name)); }) != 0)
		: (gladLoadGL([](const char* name) { return reinterpret_cast<GLADapiproc>(context_being_created->GetProcAddress(name)); }) != 0);
	if (!glad_ok)
	{
		Error::SetStringFmt(error, "Failed to load {} functions for GLAD", is_gles ? "GLES" : "GL");
		return nullptr;
	}

	context_being_created = nullptr;

	return context;
}
