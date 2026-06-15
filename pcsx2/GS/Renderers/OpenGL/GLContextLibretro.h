// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/OpenGL/GLContext.h"

#include <span>

// A GLContext that does NOT create its own GL/EGL context: it wraps the context the
// libretro frontend already created for hardware rendering. Function pointers come from
// the frontend's get_proc_address; the context is current on whatever thread the GS
// renders on (the frontend thread, where the GS ring is drained — see the libretro core),
// so MakeCurrent/DoneCurrent are no-ops. Presentation is done by the libretro core via
// video_cb(RETRO_HW_FRAME_BUFFER_VALID), so SwapBuffers is a no-op too.
//
// This mirrors Dolphin's GLContextLR and avoids the dual-EGLDisplay/KMS conflict that a
// second, self-created context causes on tiled drivers (V3D/RPi5).
class GLContextLibretro final : public GLContext
{
public:
	using GetProcFn = void* (*)(const char* name);

	GLContextLibretro(const WindowInfo& wi, GetProcFn proc, bool is_gles);
	~GLContextLibretro() override;

	static std::unique_ptr<GLContext> Create(const WindowInfo& wi, GetProcFn proc, bool is_gles);

	void* GetProcAddress(const char* name) override;
	bool ChangeSurface(const WindowInfo& new_wi) override;
	void ResizeSurface(u32 new_surface_width = 0, u32 new_surface_height = 0) override;
	bool SwapBuffers() override;
	bool IsCurrent() override;
	bool MakeCurrent() override;
	bool DoneCurrent() override;
	bool SupportsNegativeSwapInterval() const override;
	bool SetSwapInterval(s32 interval) override;
	std::unique_ptr<GLContext> CreateSharedContext(const WindowInfo& wi, Error* error) override;

	bool IsGLES() const override { return m_is_gles; }

private:
	GetProcFn m_proc = nullptr;
	bool m_is_gles = true;
};
