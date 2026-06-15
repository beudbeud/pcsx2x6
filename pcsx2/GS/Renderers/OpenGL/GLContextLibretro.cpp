// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/OpenGL/GLContextLibretro.h"

#include "common/Console.h"

GLContextLibretro::GLContextLibretro(const WindowInfo& wi, GetProcFn proc, bool is_gles)
	: GLContext(wi), m_proc(proc), m_is_gles(is_gles)
{
}

GLContextLibretro::~GLContextLibretro() = default;

std::unique_ptr<GLContext> GLContextLibretro::Create(const WindowInfo& wi, GetProcFn proc, bool is_gles)
{
	if (!proc)
		return nullptr;
	Console.WriteLnFmt("GLContextLibretro: wrapping the libretro {} HW context.", is_gles ? "GLES" : "GL");
	return std::make_unique<GLContextLibretro>(wi, proc, is_gles);
}

void* GLContextLibretro::GetProcAddress(const char* name)
{
	return m_proc ? m_proc(name) : nullptr;
}

// The libretro frontend owns the surface/context lifetime and current-ness on the render
// thread, so these are all no-ops. Presentation is video_cb() in the libretro core.
bool GLContextLibretro::ChangeSurface(const WindowInfo& new_wi)
{
	m_wi = new_wi;
	return true;
}

void GLContextLibretro::ResizeSurface(u32 new_surface_width, u32 new_surface_height)
{
	if (new_surface_width != 0)
		m_wi.surface_width = new_surface_width;
	if (new_surface_height != 0)
		m_wi.surface_height = new_surface_height;
}

bool GLContextLibretro::SwapBuffers()
{
	return true;
}

bool GLContextLibretro::IsCurrent()
{
	return true;
}

bool GLContextLibretro::MakeCurrent()
{
	return true;
}

bool GLContextLibretro::DoneCurrent()
{
	return true;
}

bool GLContextLibretro::SupportsNegativeSwapInterval() const
{
	return false;
}

bool GLContextLibretro::SetSwapInterval(s32 interval)
{
	return true;
}

std::unique_ptr<GLContext> GLContextLibretro::CreateSharedContext(const WindowInfo& wi, Error* error)
{
	// Sub-contexts (e.g. for the texture-upload thread) aren't supported through the single
	// libretro context; the GS device uses only the main context in this mode.
	return nullptr;
}
