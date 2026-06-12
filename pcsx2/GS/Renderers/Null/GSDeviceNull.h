// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/Common/GSDevice.h"
#include <vector>

// CPU-backed texture: all pixel data lives in a std::vector<u8>.
// Used by GSDeviceNull so the software renderer can run without any GPU context.
class GSTextureCPU final : public GSTexture
{
public:
	GSTextureCPU(Type type, int width, int height, int levels, Format format);
	~GSTextureCPU() override = default;

	void* GetNativeHandle() const override { return m_buffer.empty() ? nullptr : (void*)m_buffer.data(); }
	bool Update(const GSVector4i& r, const void* data, int pitch, int layer = 0) override;
	bool Map(GSMap& m, const GSVector4i* r = nullptr, int layer = 0) override;
	void Unmap() override {}
	void GenerateMipmap() override {}
#ifdef PCSX2_DEVBUILD
	void SetDebugName(std::string_view) override {}
#endif

	u8* GetBuffer() { return m_buffer.empty() ? nullptr : m_buffer.data(); }
	const u8* GetBuffer() const { return m_buffer.empty() ? nullptr : m_buffer.data(); }
	u32 GetPitch() const { return m_pitch; }

private:
	std::vector<u8> m_buffer;
	u32 m_pitch = 0;
};

// CPU-backed download texture used by PerformFramebufferReadback.
class GSDownloadTextureCPU final : public GSDownloadTexture
{
public:
	GSDownloadTextureCPU(u32 width, u32 height, GSTexture::Format format);
	~GSDownloadTextureCPU() override = default;

	void CopyFromTexture(const GSVector4i& drc, GSTexture* stex, const GSVector4i& src,
		u32 src_level, bool use_transfer_pitch = true) override;
	bool Map(const GSVector4i& read_rc) override;
	void Unmap() override {}
	void Flush() override {}
#ifdef PCSX2_DEVBUILD
	void SetDebugName(std::string_view) override {}
#endif

private:
	std::vector<u8> m_buffer;
};

// CPU-only GS device: no OpenGL, no Vulkan, all operations are done in software.
// Used for the libretro build on platforms where only OpenGL ES is available.
class GSDeviceNull final : public GSDevice
{
public:
	GSDeviceNull();
	~GSDeviceNull() override = default;

	bool Create(GSVSyncMode vsync_mode, bool allow_present_throttle) override;

	RenderAPI GetRenderAPI() const override { return RenderAPI::None; }
	bool HasSurface() const override { return false; }
	void DestroySurface() override {}
	bool UpdateWindow() override { return true; }
	void ResizeWindow(u32 new_w, u32 new_h, float new_scale) override;
	bool SupportsExclusiveFullscreen() const override { return false; }
	PresentResult BeginPresent(bool frame_skip) override { return PresentResult::FrameSkipped; }
	void EndPresent() override {}
	void SetVSyncMode(GSVSyncMode, bool) override {}
	std::string GetDriverInfo() const override { return "CPU Software (No GPU)"; }
	bool SetGPUTimingEnabled(bool) override { return false; }
	float GetAndResetAccumulatedGPUTime() override { return 0.0f; }
	void PushDebugGroup(const char*, ...) override {}
	void PopDebugGroup() override {}
	void InsertDebugMessage(DebugMessageCategory, const char*, ...) override {}

	std::unique_ptr<GSDownloadTexture> CreateDownloadTexture(u32 width, u32 height, GSTexture::Format format) override;
	void CopyRect(GSTexture* sTex, GSTexture* dTex, const GSVector4i& r, u32 destX, u32 destY) override;
	void UpdateCLUTTexture(GSTexture*, float, u32, u32, GSTexture*, u32, u32) override {}
	void ConvertToIndexedTexture(GSTexture*, float, u32, u32, u32, u32, GSTexture*, u32, u32) override {}
	void FilteredDownsampleTexture(GSTexture* sTex, GSTexture* dTex, u32 downsample_factor,
		const GSVector2i& clamp_min, const GSVector4& dRect) override;
	void RenderHW(GSHWDrawConfig&) override {}
	void ClearSamplerCache() override {}
	void PresentRect(GSTexture*, const GSVector4&, GSTexture*, const GSVector4&,
		PresentShader, float, Filter) override {}

protected:
	GSTexture* CreateSurface(GSTexture::Type type, int width, int height, int levels, GSTexture::Format format) override;
	void DoMerge(GSTexture* sTex[3], GSVector4* sRect, GSTexture* dTex, GSVector4* dRect,
		const GSRegPMODE& PMODE, const GSRegEXTBUF& EXTBUF, u32 c, const Filter filter) override;
	void DoInterlace(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
		ShaderInterlace shader, Filter filter, const InterlaceConstantBuffer& cb) override;
	void DoFXAA(GSTexture* sTex, GSTexture* dTex) override;
	void DoShadeBoost(GSTexture* sTex, GSTexture* dTex, const float params[4]) override;
	bool DoCAS(GSTexture*, GSTexture*, bool, const std::array<u32, NUM_CAS_CONSTANTS>&) override { return false; }
	void DoStretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
		ShaderConvertSelector shader, Filter filter) override;

private:
	// Blit src (CPU) into dst (CPU) using normalized UV source rect and pixel-coord dest rect.
	// nearest-neighbor sampling.
	static void CPUBlit(GSTextureCPU* src, const GSVector4& sRect,
		GSTextureCPU* dst, const GSVector4& dRect, bool set_opaque_alpha);
};
