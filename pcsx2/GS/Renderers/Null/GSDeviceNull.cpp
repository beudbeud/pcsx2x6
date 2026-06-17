// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GSDeviceNull.h"

#include "GS/GSRegs.h"
#include "Host.h"
#include "common/Console.h"

#include <algorithm>
#include <cstring>

// ---------------------------------------------------------------------------
// GSTextureCPU
// ---------------------------------------------------------------------------

GSTextureCPU::GSTextureCPU(Type type, int width, int height, int levels, Format format)
{
	m_type = type;
	m_format = format;
	m_size = GSVector2i(width, height);
	m_mipmap_levels = levels;

	// Only Color and UNorm8 are used by the software renderer readback path.
	const u32 bpp = (format == Format::UNorm8) ? 1u : 4u;
	m_pitch = static_cast<u32>(width) * bpp;
	m_buffer.assign(static_cast<size_t>(m_pitch) * static_cast<u32>(height), 0);
}

bool GSTextureCPU::Update(const GSVector4i& r, const void* data, int pitch, int layer)
{
	if (m_buffer.empty() || layer != 0)
		return false;

	const int bpp = (m_format == Format::UNorm8) ? 1 : 4;
	const int x0 = std::max(0, r.x), y0 = std::max(0, r.y);
	const int x1 = std::min(m_size.x, r.z), y1 = std::min(m_size.y, r.w);
	if (x0 >= x1 || y0 >= y1)
		return true;

	const u32 row_bytes = static_cast<u32>(x1 - x0) * bpp;
	for (int y = y0; y < y1; y++)
	{
		const u8* src_row = static_cast<const u8*>(data) + static_cast<ptrdiff_t>(y - r.y) * pitch + x0 * bpp;
		u8* dst_row = m_buffer.data() + static_cast<ptrdiff_t>(y) * m_pitch + static_cast<ptrdiff_t>(x0) * bpp;
		std::memcpy(dst_row, src_row, row_bytes);
	}
	return true;
}

bool GSTextureCPU::Map(GSMap& m, const GSVector4i* r, int layer)
{
	if (m_buffer.empty() || layer != 0)
		return false;

	const int x0 = r ? r->x : 0;
	const int y0 = r ? r->y : 0;
	const int bpp = (m_format == Format::UNorm8) ? 1 : 4;

	m.bits = m_buffer.data() + static_cast<ptrdiff_t>(y0) * m_pitch + static_cast<ptrdiff_t>(x0) * bpp;
	m.pitch = static_cast<int>(m_pitch);
	return true;
}

// ---------------------------------------------------------------------------
// GSDownloadTextureCPU
// ---------------------------------------------------------------------------

GSDownloadTextureCPU::GSDownloadTextureCPU(u32 width, u32 height, GSTexture::Format format)
	: GSDownloadTexture(width, height, format)
{
	const u32 bpp = (format == GSTexture::Format::UNorm8) ? 1u : 4u;
	m_buffer.assign(static_cast<size_t>(width) * height * bpp, 0);
}

void GSDownloadTextureCPU::CopyFromTexture(const GSVector4i& drc, GSTexture* stex,
	const GSVector4i& src, u32 src_level, bool use_transfer_pitch)
{
	GSTextureCPU* src_cpu = static_cast<GSTextureCPU*>(stex);
	if (!src_cpu || src_cpu->GetBuffer() == nullptr || m_buffer.empty())
		return;

	const u32 bpp = (src_cpu->GetFormat() == GSTexture::Format::UNorm8) ? 1u : 4u;
	const u32 dst_pitch = static_cast<u32>(drc.width()) * bpp;
	m_current_pitch = dst_pitch;

	const int sw = src_cpu->GetWidth(), sh = src_cpu->GetHeight();
	const u32 src_pitch = src_cpu->GetPitch();
	const u8* src_data = src_cpu->GetBuffer();

	const int copy_w = std::min(drc.width(), src.width());
	const int copy_h = std::min(drc.height(), src.height());

	for (int y = 0; y < copy_h; y++)
	{
		const int sy = std::clamp(src.y + y, 0, sh - 1);
		const int dy = drc.y + y;
		if (dy < 0 || static_cast<u32>(dy) >= m_height)
			continue;

		const u8* src_row = src_data + static_cast<ptrdiff_t>(sy) * src_pitch + static_cast<ptrdiff_t>(src.x) * bpp;
		u8* dst_row = m_buffer.data() + static_cast<ptrdiff_t>(dy) * dst_pitch + static_cast<ptrdiff_t>(drc.x) * bpp;
		std::memcpy(dst_row, src_row, static_cast<size_t>(copy_w) * bpp);
	}
	m_needs_flush = false;
}

bool GSDownloadTextureCPU::Map(const GSVector4i& read_rc)
{
	if (m_buffer.empty())
		return false;

	const u32 bpp = (m_format == GSTexture::Format::UNorm8) ? 1u : 4u;
	m_current_pitch = m_width * bpp;
	m_map_pointer = m_buffer.data();
	return true;
}

// ---------------------------------------------------------------------------
// GSDeviceNull
// ---------------------------------------------------------------------------

GSDeviceNull::GSDeviceNull() = default;

bool GSDeviceNull::Create(GSVSyncMode vsync_mode, bool allow_present_throttle)
{
	if (!GSDevice::Create(vsync_mode, allow_present_throttle))
		return false;

	// Use the libretro host window info (Surfaceless, 640x480) so ImGui can initialize.
	if (!AcquireWindow(false))
		return false;

	Console.WriteLn("GSDeviceNull: CPU-only device created (%ux%u)", m_window_info.surface_width, m_window_info.surface_height);
	return true;
}

void GSDeviceNull::ResizeWindow(u32 new_w, u32 new_h, float new_scale)
{
	m_window_info.surface_width = new_w;
	m_window_info.surface_height = new_h;
	m_window_info.surface_scale = new_scale;
}

GSTexture* GSDeviceNull::CreateSurface(GSTexture::Type type, int width, int height, int levels, GSTexture::Format format)
{
	return new GSTextureCPU(type, std::max(width, 1), std::max(height, 1), std::max(levels, 1), format);
}

std::unique_ptr<GSDownloadTexture> GSDeviceNull::CreateDownloadTexture(u32 width, u32 height, GSTexture::Format format)
{
	return std::make_unique<GSDownloadTextureCPU>(width, height, format);
}

void GSDeviceNull::CopyRect(GSTexture* sTex, GSTexture* dTex, const GSVector4i& r, u32 destX, u32 destY)
{
	GSTextureCPU* src = static_cast<GSTextureCPU*>(sTex);
	GSTextureCPU* dst = static_cast<GSTextureCPU*>(dTex);
	if (!src || !dst || !src->GetBuffer() || !dst->GetBuffer())
		return;

	const int bpp = (src->GetFormat() == GSTexture::Format::UNorm8) ? 1 : 4;
	const u32 row_bytes = static_cast<u32>(r.width()) * bpp;

	for (int y = r.y; y < r.w; y++)
	{
		if (y < 0 || y >= src->GetHeight())
			continue;
		const int dy = static_cast<int>(destY) + (y - r.y);
		if (dy < 0 || dy >= dst->GetHeight())
			continue;

		const u8* src_row = src->GetBuffer() + static_cast<ptrdiff_t>(y) * src->GetPitch() + static_cast<ptrdiff_t>(r.x) * bpp;
		u8* dst_row = dst->GetBuffer() + static_cast<ptrdiff_t>(dy) * dst->GetPitch() + static_cast<ptrdiff_t>(destX) * bpp;
		std::memcpy(dst_row, src_row, row_bytes);
	}
}

// Blit src (CPU) into dst (CPU) using normalized UV source rect and pixel-coord dest rect.
void GSDeviceNull::CPUBlit(GSTextureCPU* src, const GSVector4& sRect,
	GSTextureCPU* dst, const GSVector4& dRect, bool set_opaque_alpha)
{
	if (!src || !dst || !src->GetBuffer() || !dst->GetBuffer())
		return;

	const int sw = src->GetWidth(), sh = src->GetHeight();
	const int dw = dst->GetWidth(), dh = dst->GetHeight();
	const u32 src_pitch = src->GetPitch();
	const u32 dst_pitch = dst->GetPitch();
	const u8* src_data = src->GetBuffer();
	u8* dst_data = dst->GetBuffer();

	const int dx0 = std::max(0, (int)dRect.x);
	const int dy0 = std::max(0, (int)dRect.y);
	const int dx1 = std::min(dw, (int)std::ceilf(dRect.z));
	const int dy1 = std::min(dh, (int)std::ceilf(dRect.w));
	if (dx0 >= dx1 || dy0 >= dy1)
		return;

	const float dst_rw = static_cast<float>(dx1 - dx0);
	const float dst_rh = static_cast<float>(dy1 - dy0);

	// Fast path: 1:1 copy with no scaling and full source UV
	const bool full_src = (sRect.x == 0.0f && sRect.y == 0.0f && sRect.z == 1.0f && sRect.w == 1.0f);
	const bool same_size = ((dx1 - dx0) == sw && (dy1 - dy0) == sh);

	if (full_src && same_size && !set_opaque_alpha)
	{
		for (int y = dy0; y < dy1; y++)
		{
			const int sy = y - dy0;
			if (sy >= sh) break;
			std::memcpy(dst_data + static_cast<ptrdiff_t>(y) * dst_pitch + static_cast<ptrdiff_t>(dx0) * 4,
				src_data + static_cast<ptrdiff_t>(sy) * src_pitch, static_cast<size_t>(sw) * 4);
		}
		return;
	}

	for (int y = dy0; y < dy1; y++)
	{
		const float fy = (y - dy0) / dst_rh;
		const int sy = std::clamp((int)((sRect.y + (sRect.w - sRect.y) * fy) * sh), 0, sh - 1);
		const u8* src_row = src_data + static_cast<ptrdiff_t>(sy) * src_pitch;
		u32* dst_row = reinterpret_cast<u32*>(dst_data + static_cast<ptrdiff_t>(y) * dst_pitch) + dx0;

		for (int x = dx0; x < dx1; x++)
		{
			const float fx = (x - dx0) / dst_rw;
			const int sx = std::clamp((int)((sRect.x + (sRect.z - sRect.x) * fx) * sw), 0, sw - 1);
			u32 pixel = *reinterpret_cast<const u32*>(src_row + static_cast<ptrdiff_t>(sx) * 4);
			if (set_opaque_alpha)
				pixel |= 0xFF000000u;
			*dst_row++ = pixel;
		}
	}
}

void GSDeviceNull::DoStretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex,
	const GSVector4& dRect, ShaderConvertSelector shader, Filter filter)
{
	GSTextureCPU* src = static_cast<GSTextureCPU*>(sTex);
	GSTextureCPU* dst = static_cast<GSTextureCPU*>(dTex);

	if (sTex && sTex->GetState() == GSTexture::State::Cleared)
	{
		// Source is a solid color; fill the destination rectangle.
		if (!dst || !dst->GetBuffer()) return;
		const u32 color = sTex->GetClearColor();
		const int dx0 = std::max(0, (int)dRect.x);
		const int dy0 = std::max(0, (int)dRect.y);
		const int dx1 = std::min(dst->GetWidth(), (int)std::ceilf(dRect.z));
		const int dy1 = std::min(dst->GetHeight(), (int)std::ceilf(dRect.w));
		for (int y = dy0; y < dy1; y++)
		{
			u32* row = reinterpret_cast<u32*>(dst->GetBuffer() + static_cast<ptrdiff_t>(y) * dst->GetPitch()) + dx0;
			std::fill(row, row + (dx1 - dx0), color);
		}
		return;
	}

	// TRANSPARENCY_FILTER: copies pixels and forces alpha to 0xFF for display output.
	const bool set_alpha = (shader.Shader() == ShaderConvert::TRANSPARENCY_FILTER);
	CPUBlit(src, sRect, dst, dRect, set_alpha);
}

void GSDeviceNull::DoMerge(GSTexture* sTex[3], GSVector4* sRect, GSTexture* dTex, GSVector4* dRect,
	const GSRegPMODE& PMODE, const GSRegEXTBUF& EXTBUF, u32 c, const Filter filter)
{
	// Clear destination to background colour.
	ClearRenderTarget(dTex, c);

	// Composite both display circuits. tex[0] = RC1, tex[1] = RC2.
	for (int i = 0; i < 2; i++)
	{
		if (!sTex[i]) continue;
		GSTextureCPU* src = static_cast<GSTextureCPU*>(sTex[i]);
		GSTextureCPU* dst = static_cast<GSTextureCPU*>(dTex);
		CPUBlit(src, sRect[i], dst, dRect[i], false);
	}
}

void GSDeviceNull::DoInterlace(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex,
	const GSVector4& dRect, ShaderInterlace shader, Filter filter, const InterlaceConstantBuffer& cb)
{
	// Simple pass-through: just copy the source to the destination.
	GSTextureCPU* src = static_cast<GSTextureCPU*>(sTex);
	GSTextureCPU* dst = static_cast<GSTextureCPU*>(dTex);
	CPUBlit(src, sRect, dst, dRect, false);
}

void GSDeviceNull::DoFXAA(GSTexture* sTex, GSTexture* dTex)
{
	// Pass-through: FXAA is a GPU-only effect.
	GSTextureCPU* src = static_cast<GSTextureCPU*>(sTex);
	GSTextureCPU* dst = static_cast<GSTextureCPU*>(dTex);
	CPUBlit(src, GSVector4(0.0f, 0.0f, 1.0f, 1.0f), dst,
		GSVector4(0.0f, 0.0f, static_cast<float>(dst->GetWidth()), static_cast<float>(dst->GetHeight())), false);
}

void GSDeviceNull::DoShadeBoost(GSTexture* sTex, GSTexture* dTex, const float params[4])
{
	// Pass-through: no shade boost in CPU mode.
	GSTextureCPU* src = static_cast<GSTextureCPU*>(sTex);
	GSTextureCPU* dst = static_cast<GSTextureCPU*>(dTex);
	CPUBlit(src, GSVector4(0.0f, 0.0f, 1.0f, 1.0f), dst,
		GSVector4(0.0f, 0.0f, static_cast<float>(dst->GetWidth()), static_cast<float>(dst->GetHeight())), false);
}

void GSDeviceNull::FilteredDownsampleTexture(GSTexture* sTex, GSTexture* dTex,
	u32 downsample_factor, const GSVector2i& clamp_min, const GSVector4& dRect)
{
	GSTextureCPU* src = static_cast<GSTextureCPU*>(sTex);
	GSTextureCPU* dst = static_cast<GSTextureCPU*>(dTex);
	CPUBlit(src, GSVector4(0.0f, 0.0f, 1.0f, 1.0f), dst, dRect, false);
}
