// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/OpenGL/GLContext.h"

#include <span>

// A pure-GLX offscreen (pbuffer) GL context for the headless/libretro (Surfaceless) case
// on X11 desktops. Unlike GLContextEGL*, this uses GLX, not EGL — required when the
// libretro frontend (RetroArch video_driver=gl/glcore on X11) holds a GLX context: on
// NVIDIA, a current GLX context anywhere in the process blocks eglMakeCurrent() for any
// EGL context, so the GS could never get a usable GL context via EGL. Two independent
// GLX contexts coexist fine, so the GS uses GLX too.
//
// libX11/libGL are loaded with dlopen (no hard link dependency): on a KMS/headless system
// with no X server (e.g. Recalbox), Create() returns nullptr and GLContext::Create falls
// back to the EGL/GBM path.
//
// Optionally creates its context sharing the libretro frontend's GLX context (set via
// SetShareContext), so the composited GS texture is usable directly in the frontend's
// context for a zero-copy present.
class GLContextGLX final : public GLContext
{
public:
	GLContextGLX(const WindowInfo& wi);
	~GLContextGLX() override;

	static std::unique_ptr<GLContext> Create(const WindowInfo& wi, std::span<const Version> versions_to_try, Error* error);

	// Register the frontend's GLX context + display to share object namespaces with (zero-copy
	// HW render). Pass nullptr to disable. Captured by the libretro core in context_reset.
	static void SetShareContext(void* glx_context, void* display);

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

	bool IsGLES() const override { return false; }

private:
	bool Initialize(std::span<const Version> versions_to_try, Error* error);

	// Opaque handles (kept as void* so the public header doesn't pull in X11/GLX types).
	void* m_display = nullptr; // Display*
	void* m_fb_config = nullptr; // GLXFBConfig
	void* m_context = nullptr; // GLXContext
	unsigned long m_pbuffer = 0; // GLXPbuffer
	bool m_owns_display = false; // false when using the frontend's captured display
};
