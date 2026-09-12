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

#include "stdafx.h"
#include <moreuuids.h>
#include "DSUtil/Utils.h"
#include "CaptureColorInfoFilter.h"

static const BITMAPINFOHEADER* GetBIH(const CMediaType* pmt)
{
	return GetBitmapInfoHeader(pmt);
}

CCaptureColorInfoFilter::CCaptureColorInfoFilter(LPUNKNOWN punk, HRESULT* phr, const DXVA2_ExtendedFormat& exfmt)
	: CTransformFilter(L"CCaptureColorInfoFilter", punk, __uuidof(CCaptureColorInfoFilter))
	, m_exfmt(exfmt)
{
	if (phr) {
		*phr = S_OK;
	}
}

HRESULT CCaptureColorInfoFilter::CheckConnect(PIN_DIRECTION dir, IPin* pPin)
{
	return GetCLSID(pPin) == __uuidof(*this) ? E_FAIL : S_OK;
}

HRESULT CCaptureColorInfoFilter::CheckInputType(const CMediaType* mtIn)
{
	if (mtIn->majortype != MEDIATYPE_Video) {
		return VFW_E_TYPE_NOT_ACCEPTED;
	}

	if (mtIn->formattype != FORMAT_VideoInfo && mtIn->formattype != FORMAT_VideoInfo2) {
		return VFW_E_TYPE_NOT_ACCEPTED;
	}

	const BITMAPINFOHEADER* pBIH = GetBIH(mtIn);
	if (!pBIH || pBIH->biWidth <= 0 || pBIH->biHeight == 0) {
		return VFW_E_TYPE_NOT_ACCEPTED;
	}

	return S_OK;
}

HRESULT CCaptureColorInfoFilter::MakeTaggedType(const CMediaType* mtIn, CMediaType* pmtOut)
{
	*pmtOut = *mtIn;

	if (pmtOut->formattype == FORMAT_VideoInfo) {
		HRESULT hr = ConvertVideoInfoToVideoInfo2(pmtOut);
		if (FAILED(hr)) {
			return hr;
		}
	}

	if (pmtOut->formattype != FORMAT_VideoInfo2 || pmtOut->cbFormat < sizeof(VIDEOINFOHEADER2)) {
		return VFW_E_TYPE_NOT_ACCEPTED;
	}

	VIDEOINFOHEADER2* vih2 = (VIDEOINFOHEADER2*)pmtOut->pbFormat;

	LONG x = vih2->bmiHeader.biWidth;
	LONG y = abs(vih2->bmiHeader.biHeight);
	ReduceDim(x, y);
	if (x > 0 && y > 0) {
		vih2->dwPictAspectRatioX = x;
		vih2->dwPictAspectRatioY = y;
	}

	// capture streams from HDMI sources are progressive
	vih2->dwInterlaceFlags = 0;
	vih2->dwControlFlags   = m_exfmt.value;

	if (vih2->bmiHeader.biSizeImage) {
		pmtOut->SetSampleSize(vih2->bmiHeader.biSizeImage);
	}

	return S_OK;
}

HRESULT CCaptureColorInfoFilter::GetMediaType(int iPosition, CMediaType* pmt)
{
	if (m_pInput->IsConnected() == FALSE) {
		return E_UNEXPECTED;
	}

	if (iPosition < 0) {
		return E_INVALIDARG;
	}

	if (iPosition > 0) {
		return VFW_S_NO_MORE_ITEMS;
	}

	return MakeTaggedType(&m_pInput->CurrentMediaType(), pmt);
}

HRESULT CCaptureColorInfoFilter::CheckTransform(const CMediaType* mtIn, const CMediaType* mtOut)
{
	if (mtOut->majortype != MEDIATYPE_Video || mtOut->subtype != mtIn->subtype) {
		return VFW_E_TYPE_NOT_ACCEPTED;
	}

	// the whole point of this filter is the VIDEOINFOHEADER2 color information
	if (mtOut->formattype != FORMAT_VideoInfo2) {
		return VFW_E_TYPE_NOT_ACCEPTED;
	}

	const BITMAPINFOHEADER* pBIHIn  = GetBIH(mtIn);
	const BITMAPINFOHEADER* pBIHOut = GetBIH(mtOut);

	if (!pBIHIn || !pBIHOut) {
		return VFW_E_TYPE_NOT_ACCEPTED;
	}

	// reject re-negotiated (e.g. stride aligned) types, the data is copied 1:1
	if (pBIHIn->biWidth       != pBIHOut->biWidth
			|| abs(pBIHIn->biHeight)  != abs(pBIHOut->biHeight)
			|| pBIHIn->biCompression  != pBIHOut->biCompression
			|| pBIHIn->biBitCount     != pBIHOut->biBitCount) {
		return VFW_E_TYPE_NOT_ACCEPTED;
	}

	if (pBIHIn->biSizeImage && pBIHOut->biSizeImage && pBIHIn->biSizeImage != pBIHOut->biSizeImage) {
		return VFW_E_TYPE_NOT_ACCEPTED;
	}

	return S_OK;
}

HRESULT CCaptureColorInfoFilter::DecideBufferSize(IMemAllocator* pAllocator, ALLOCATOR_PROPERTIES* pProperties)
{
	if (m_pInput->IsConnected() == FALSE) {
		return E_UNEXPECTED;
	}

	const CMediaType& mtIn  = m_pInput->CurrentMediaType();
	const CMediaType& mtOut = m_pOutput->CurrentMediaType();

	const BITMAPINFOHEADER* pBIHOut = GetBIH(&mtOut);
	if (!pBIHOut) {
		return E_UNEXPECTED;
	}

	LONG cbBuffer = (LONG)pBIHOut->biSizeImage;

	if (const BITMAPINFOHEADER* pBIHIn = GetBIH(&mtIn)) {
		cbBuffer = std::max(cbBuffer, (LONG)pBIHIn->biSizeImage);
	}
	cbBuffer = std::max(cbBuffer, (LONG)mtIn.GetSampleSize());

	if (cbBuffer <= 0) {
		cbBuffer = (LONG)(((int64_t)pBIHOut->biWidth * abs(pBIHOut->biHeight)
						   * std::max(pBIHOut->biBitCount, (WORD)8)) >> 3);
	}

	if (cbBuffer <= 0) {
		return E_UNEXPECTED;
	}

	pProperties->cBuffers = std::max(pProperties->cBuffers, 1L);
	pProperties->cbBuffer = cbBuffer;
	pProperties->cbAlign  = 1;
	pProperties->cbPrefix = 0;

	HRESULT hr;
	ALLOCATOR_PROPERTIES Actual;

	if (FAILED(hr = pAllocator->SetProperties(pProperties, &Actual))) {
		return hr;
	}

	return pProperties->cBuffers > Actual.cBuffers || pProperties->cbBuffer > Actual.cbBuffer
		   ? E_FAIL
		   : NOERROR;
}

HRESULT CCaptureColorInfoFilter::Transform(IMediaSample* pIn, IMediaSample* pOut)
{
	HRESULT hr;

	// A media type attached to the input sample means the upstream filter changed
	// format on the fly. Tag the new type and hand it downstream as well.
	{
		AM_MEDIA_TYPE* pmt = nullptr;
		if (SUCCEEDED(pIn->GetMediaType(&pmt)) && pmt) {
			CMediaType mtIn = *pmt;
			DeleteMediaType(pmt);

			CMediaType mtOut;
			if (SUCCEEDED(MakeTaggedType(&mtIn, &mtOut))) {
				m_pInput->SetMediaType(&mtIn);
				m_pOutput->SetMediaType(&mtOut);
				pOut->SetMediaType(&mtOut);
			}
		}
	}

	{
		AM_MEDIA_TYPE* pmt = nullptr;
		if (SUCCEEDED(pOut->GetMediaType(&pmt)) && pmt) {
			CMediaType mt = *pmt;
			DeleteMediaType(pmt);
			m_pOutput->SetMediaType(&mt);
		}
	}

	BYTE* pDataIn = nullptr;
	BYTE* pDataOut = nullptr;

	if (FAILED(hr = pIn->GetPointer(&pDataIn)) || !pDataIn) {
		return S_FALSE;
	}
	if (FAILED(hr = pOut->GetPointer(&pDataOut)) || !pDataOut) {
		return hr;
	}

	const long len = pIn->GetActualDataLength();
	if (len <= 0) {
		pOut->SetActualDataLength(0);
		return S_OK;
	}

	if (len > pOut->GetSize()) {
		// must never overrun the downstream buffer
		return S_FALSE;
	}

	memcpy(pDataOut, pDataIn, len);
	pOut->SetActualDataLength(len);

	return S_OK;
}
