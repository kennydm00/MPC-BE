/*
 * (C) 2026 see Authors.txt
 *
 * This file is part of MPC-BE.
 *
 * MPC-BE is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * MPC-BE is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#pragma once

#include <dxva2api.h>

//
// Tags the media type of a capture stream with explicit colorimetry.
//
// Capture drivers usually offer a plain VIDEOINFOHEADER, which carries no color
// information at all, so renderers have to guess (and guess BT.709 / 16-235).
// This filter converts the type to VIDEOINFOHEADER2 and fills dwControlFlags with
// a DXVA2_ExtendedFormat marked as AMCONTROL_COLORINFO_PRESENT. The pixel data is
// passed through untouched.
//
// It is inserted between the capture Smart Tee and the video renderer, and only
// when the user explicitly enabled the option for that device.
//
class __declspec(uuid("1D6A1C39-7B1D-4F9A-9E9B-4B0B1B7B5E21"))
	CCaptureColorInfoFilter : public CTransformFilter
{
	DXVA2_ExtendedFormat m_exfmt = {};

	// Builds the tagged output type from an input type.
	HRESULT MakeTaggedType(const CMediaType* mtIn, CMediaType* pmtOut);

protected:
	HRESULT CheckConnect(PIN_DIRECTION dir, IPin* pPin);
	HRESULT CheckInputType(const CMediaType* mtIn);
	HRESULT CheckTransform(const CMediaType* mtIn, const CMediaType* mtOut);
	HRESULT Transform(IMediaSample* pIn, IMediaSample* pOut);
	HRESULT DecideBufferSize(IMemAllocator* pAllocator, ALLOCATOR_PROPERTIES* pProperties);
	HRESULT GetMediaType(int iPosition, CMediaType* pmt);

public:
	CCaptureColorInfoFilter(LPUNKNOWN punk, HRESULT* phr, const DXVA2_ExtendedFormat& exfmt);
};
