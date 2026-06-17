// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"
#include "common/WindowInfo.h"

#include <array>
#include <memory>
#include <vector>

class Error;

class GLContext
{
public:
	GLContext(const WindowInfo& wi);
	virtual ~GLContext();

	struct Version
	{
		int major_version;
		int minor_version;
		bool is_gles = false;
	};

	// Zero-copy HW render: a single-plane dmabuf exported from a GL texture, for the
	// libretro frontend context to import. fd is owned by the caller (close it).
	struct DmaBufFrame
	{
		int fd = -1;
		u32 width = 0;
		u32 height = 0;
		u32 stride = 0;
		u32 offset = 0;
		u32 fourcc = 0;
		u64 modifier = 0;
	};

	// Export a GL 2D texture as a dmabuf via EGL_MESA_image_dma_buf_export. Returns false
	// if unsupported. Implemented by the EGL context; called on the GS thread (context current).
	virtual bool ExportTextureDMABUF(u32 texture_id, DmaBufFrame* out) { return false; }

	// Allocate a LINEAR dmabuf (via GBM) and import it as a GL 2D texture in this context, to
	// render into. Linear so a consumer on another EGLDisplay can sample it without GPU-tiling
	// (UIF) ambiguity. Fills `out` (out->fd owned by this context) + the GL texture id. Returns
	// false if unsupported. Called on the GS thread (its context current).
	virtual bool CreateLinearDmaBufTexture(u32 width, u32 height, DmaBufFrame* out, u32* out_texture) { return false; }
	virtual void DestroyLinearDmaBufTexture() {}

	__fi const WindowInfo& GetWindowInfo() const { return m_wi; }

	virtual bool IsGLES() const { return false; }
	__fi u32 GetSurfaceWidth() const { return m_wi.surface_width; }
	__fi u32 GetSurfaceHeight() const { return m_wi.surface_height; }

	virtual void* GetProcAddress(const char* name) = 0;
	virtual bool ChangeSurface(const WindowInfo& new_wi) = 0;
	virtual void ResizeSurface(u32 new_surface_width = 0, u32 new_surface_height = 0) = 0;
	virtual bool SwapBuffers() = 0;
	virtual bool IsCurrent() = 0;
	virtual bool MakeCurrent() = 0;
	virtual bool DoneCurrent() = 0;
	virtual bool SupportsNegativeSwapInterval() const = 0;
	virtual bool SetSwapInterval(s32 interval) = 0;
	virtual std::unique_ptr<GLContext> CreateSharedContext(const WindowInfo& wi, Error* error) = 0;

	static std::unique_ptr<GLContext> Create(const WindowInfo& wi, Error* error);

protected:
	WindowInfo m_wi;
	Version m_version = {};
};
