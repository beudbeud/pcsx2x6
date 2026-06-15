// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/OpenGL/GLContext.h"

#include "glad/egl.h"

#include <span>

class GLContextEGL : public GLContext
{
public:
	GLContextEGL(const WindowInfo& wi);
	~GLContextEGL() override;

	static std::unique_ptr<GLContext> Create(const WindowInfo& wi, std::span<const Version> versions_to_try, Error* error);

	void* GetProcAddress(const char* name) override;
	virtual bool ChangeSurface(const WindowInfo& new_wi) override;
	virtual void ResizeSurface(u32 new_surface_width = 0, u32 new_surface_height = 0) override;
	bool SwapBuffers() override;
	bool IsCurrent() override;
	bool MakeCurrent() override;
	bool DoneCurrent() override;
	bool SupportsNegativeSwapInterval() const override;
	bool SetSwapInterval(s32 interval) override;
	virtual std::unique_ptr<GLContext> CreateSharedContext(const WindowInfo& wi, Error* error) override;

	bool ExportTextureDMABUF(u32 texture_id, DmaBufFrame* out) override;
	bool CreateLinearDmaBufTexture(u32 width, u32 height, DmaBufFrame* out, u32* out_texture) override;
	void DestroyLinearDmaBufTexture() override;

protected:
	virtual EGLDisplay GetPlatformDisplay(Error* error);
	virtual EGLSurface CreatePlatformSurface(EGLConfig config, void* win, Error* error);

	EGLDisplay TryGetPlatformDisplay(EGLenum platform, const char* platform_ext);
	EGLDisplay TryGBMPlatformDisplay(Error* error);
	EGLSurface TryCreatePlatformSurface(EGLConfig config, void* window, Error* error);
	EGLDisplay GetFallbackDisplay(Error* error);
	EGLSurface CreateFallbackSurface(EGLConfig config, void* window, Error* error);

	bool Initialize(std::span<const Version> versions_to_try, Error* error);
	bool CreateContext(const Version& version, EGLContext share_context);
	bool CreateContextAndSurface(const Version& version, EGLContext share_context, bool make_current);
	bool CreateSurface();
	bool CreatePBufferSurface();
	bool CheckConfigSurfaceFormat(EGLConfig config);
	void DestroyContext();
	void DestroySurface();

	EGLDisplay m_display = EGL_NO_DISPLAY;
	EGLSurface m_surface = EGL_NO_SURFACE;
	EGLContext m_context = EGL_NO_CONTEXT;

	EGLConfig m_config = {};

	bool m_use_ext_platform_base = false;
	bool m_supports_negative_swap_interval = false;
	bool m_is_gles = false;

	bool IsGLES() const override { return m_is_gles; }

	// GBM resources used when EGL_MESA_platform_surfaceless doesn't support desktop GL
	int m_gbm_fd = -1;
	void* m_gbm_lib = nullptr;
	void* m_gbm_device = nullptr;

	// Linear dmabuf render target (zero-copy HW render): a GBM bo + EGLImage + GL texture the GS
	// blits each frame into, exported to the libretro frontend. Owned here; freed on destroy/resize.
	void* m_lin_bo = nullptr;
	void* m_lin_image = nullptr; // EGLImageKHR
	u32 m_lin_tex = 0;
	int m_lin_fd = -1;
};
