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
#include <d3d9.h>    // dxva2api.h needs the D3D9 types
#include <dxva2api.h>
#include <evr.h>
#include <vector>
#include <algorithm>
#include "DSUtil/Utils.h"
#include "SettingsDefines.h"
#include "CaptureFormat.h"

#define IDS_R_CAPTURE_DEVICES IDS_R_CAPTURE L"\\Devices"

namespace {

	// FNV-1a over the UTF-16 code units. The display name itself is stored and
	// verified, so collisions are harmless.
	uint64_t HashDisplayName(const CStringW& name)
	{
		uint64_t hash = 0xcbf29ce484222325ull;
		for (int i = 0; i < name.GetLength(); i++) {
			const wchar_t c = name[i];
			hash = (hash ^ (uint64_t)(c & 0xff)) * 0x100000001b3ull;
			hash = (hash ^ (uint64_t)((c >> 8) & 0xff)) * 0x100000001b3ull;
		}
		return hash;
	}

	CStringW FourCCString(const AM_MEDIA_TYPE* pmt)
	{
		DWORD fourcc = 0;

		if (const BITMAPINFOHEADER* pBIH = GetBitmapInfoHeader(pmt)) {
			fourcc = pBIH->biCompression;
		}
		if (fourcc <= 3) {
			// RGB/BI_BITFIELDS, use the subtype FOURCC when it looks like one
			if (pmt && pmt->subtype.Data2 == 0x0000 && pmt->subtype.Data3 == 0x0010) {
				fourcc = pmt->subtype.Data1;
			}
		}

		CStringW str;
		const BYTE* p = (const BYTE*)&fourcc;
		for (int i = 0; i < 4; i++) {
			if (p[i] >= 0x20 && p[i] < 0x7f) {
				str.AppendChar((wchar_t)p[i]);
			}
		}

		return str;
	}

	struct Candidate {
		CMediaType                mt;
		VIDEO_STREAM_CONFIG_CAPS  caps = {};
		int                       index     = 0;
		int                       sizeScore = 0;
		int                       fpsScore  = 0;
		int                       fmtScore  = 0;

		bool operator<(const Candidate& o) const {
			if (sizeScore != o.sizeScore) return sizeScore < o.sizeScore;
			if (fpsScore  != o.fpsScore)  return fpsScore  < o.fpsScore;
			if (fmtScore  != o.fmtScore)  return fmtScore  < o.fmtScore;
			return index < o.index;
		}
	};

	BITMAPINFOHEADER* GetBIHPtr(AM_MEDIA_TYPE* pmt)
	{
		if (!pmt || !pmt->pbFormat) {
			return nullptr;
		}
		if (pmt->formattype == FORMAT_VideoInfo && pmt->cbFormat >= sizeof(VIDEOINFOHEADER)) {
			return &((VIDEOINFOHEADER*)pmt->pbFormat)->bmiHeader;
		}
		if (pmt->formattype == FORMAT_VideoInfo2 && pmt->cbFormat >= sizeof(VIDEOINFOHEADER2)) {
			return &((VIDEOINFOHEADER2*)pmt->pbFormat)->bmiHeader;
		}
		return nullptr;
	}

	void SetAvgTimePerFrame(AM_MEDIA_TYPE* pmt, REFERENCE_TIME atpf)
	{
		if (pmt->formattype == FORMAT_VideoInfo) {
			((VIDEOINFOHEADER*)pmt->pbFormat)->AvgTimePerFrame = atpf;
		} else if (pmt->formattype == FORMAT_VideoInfo2) {
			((VIDEOINFOHEADER2*)pmt->pbFormat)->AvgTimePerFrame = atpf;
		}
	}

	// Rewrites the frame size of a candidate that only covers the wanted size through
	// its VIDEO_STREAM_CONFIG_CAPS range.
	void ApplyFrameSize(CMediaType& mt, int width, int height)
	{
		BITMAPINFOHEADER* pBIH = GetBIHPtr(&mt);
		if (!pBIH) {
			return;
		}

		const LONG oldW = pBIH->biWidth;
		const LONG oldH = abs(pBIH->biHeight);
		const LONG oldSize = pBIH->biSizeImage;

		pBIH->biWidth  = width;
		pBIH->biHeight = (pBIH->biHeight < 0) ? -height : height;

		if (oldSize && oldW > 0 && oldH > 0) {
			// keep the driver's own packing (padding, extra planes, ...)
			pBIH->biSizeImage = (DWORD)RescaleI64(oldSize, (int64_t)width * height, (int64_t)oldW * oldH);
		} else {
			pBIH->biSizeImage = (DWORD)(((int64_t)width * height * std::max(pBIH->biBitCount, (WORD)8)) >> 3);
		}

		if (mt.bFixedSizeSamples) {
			mt.lSampleSize = pBIH->biSizeImage;
		}

		RECT* prcSource = nullptr;
		RECT* prcTarget = nullptr;

		if (mt.formattype == FORMAT_VideoInfo) {
			VIDEOINFOHEADER* vih = (VIDEOINFOHEADER*)mt.pbFormat;
			prcSource = &vih->rcSource;
			prcTarget = &vih->rcTarget;
		} else if (mt.formattype == FORMAT_VideoInfo2) {
			VIDEOINFOHEADER2* vih2 = (VIDEOINFOHEADER2*)mt.pbFormat;
			prcSource = &vih2->rcSource;
			prcTarget = &vih2->rcTarget;

			// dwPictAspectRatio is the display aspect ratio, not the pixel geometry:
			// a device that rescales keeps the aspect of its source. Only derive it
			// from the frame size when the driver left it unset.
			if (!vih2->dwPictAspectRatioX || !vih2->dwPictAspectRatioY) {
				LONG x = width, y = height;
				ReduceDim(x, y);
				vih2->dwPictAspectRatioX = x;
				vih2->dwPictAspectRatioY = y;
			}
		}

		// Retarget only a rectangle that covers the whole old frame. One that
		// describes a real crop has to survive the resize untouched.
		const RECT rc = { 0, 0, width, height };
		if (prcSource && !IsRectEmpty(prcSource)
				&& prcSource->left == 0 && prcSource->top == 0
				&& prcSource->right == oldW && prcSource->bottom == oldH) {
			*prcSource = rc;
		}
		if (prcTarget && !IsRectEmpty(prcTarget)
				&& prcTarget->left == 0 && prcTarget->top == 0
				&& prcTarget->right == oldW && prcTarget->bottom == oldH) {
			*prcTarget = rc;
		}
	}

} // namespace

CStringW CaptureDeviceSection(const CStringW& displayName)
{
	CStringW section;
	section.Format(L"%s\\%016llX", IDS_R_CAPTURE_DEVICES, HashDisplayName(displayName));
	return section;
}

bool LoadCaptureDeviceSettings(const CStringW& displayName, CaptureDeviceSettings& cds)
{
	cds = CaptureDeviceSettings();
	cds.displayName = displayName;

	if (displayName.IsEmpty()) {
		return false;
	}

	CProfile& profile = AfxGetProfile();
	const CStringW section = CaptureDeviceSection(displayName);

	CStringW storedName;
	if (!profile.ReadString(section, L"DisplayName", storedName) || storedName != displayName) {
		return false;
	}

	int    val  = 0;
	__int64 val64 = 0;
	CStringW str;

	if (profile.ReadInt(section, L"FormatMode", val)) {
		cds.bExplicit = (val != 0);
	}
	if (profile.ReadInt(section, L"ForceHDR", val)) {
		cds.bForceHDR = (val != 0);
	}
	if (profile.ReadString(section, L"Subtype", str)) {
		cds.subtype = GUIDFromCString(str);
	}
	profile.ReadString(section, L"FourCC", cds.fourcc);
	if (profile.ReadInt(section, L"Width", val)) {
		cds.width = val;
	}
	if (profile.ReadInt(section, L"Height", val)) {
		cds.height = abs(val);
	}
	if (profile.ReadInt64(section, L"FrameInterval", val64) && val64 > 0) {
		cds.frameInterval = val64;
	}

	if (cds.subtype == GUID_NULL || cds.width <= 0 || cds.height <= 0) {
		cds.bExplicit = false;
	}

	CaptureDiag(L"CaptureFormat: loaded settings for '%s': explicit=%d hdr=%d %s %dx%d atpf=%lld",
				displayName.GetString(), (int)cds.bExplicit, (int)cds.bForceHDR,
				cds.fourcc.GetString(), cds.width, cds.height, cds.frameInterval);

	return true;
}

void SaveCaptureDeviceSettings(const CaptureDeviceSettings& cds)
{
	if (cds.displayName.IsEmpty()) {
		return;
	}

	if (cds.IsDefault()) {
		DeleteCaptureDeviceSettings(cds.displayName);
		return;
	}

	CProfile& profile = AfxGetProfile();
	const CStringW section = CaptureDeviceSection(cds.displayName);

	profile.WriteString(section, L"DisplayName", cds.displayName);
	profile.WriteInt(section, L"FormatMode", cds.bExplicit ? 1 : 0);
	profile.WriteInt(section, L"ForceHDR", cds.bForceHDR ? 1 : 0);

	if (cds.subtype != GUID_NULL) {
		profile.WriteString(section, L"Subtype", CStringFromGUID(cds.subtype));
		profile.WriteString(section, L"FourCC", cds.fourcc);
		profile.WriteInt(section, L"Width", cds.width);
		profile.WriteInt(section, L"Height", cds.height);
		profile.WriteInt64(section, L"FrameInterval", cds.frameInterval);
	}

	CaptureDiag(L"CaptureFormat: saved settings for '%s': explicit=%d hdr=%d %s %dx%d atpf=%lld",
				cds.displayName.GetString(), (int)cds.bExplicit, (int)cds.bForceHDR,
				cds.fourcc.GetString(), cds.width, cds.height, cds.frameInterval);
}

void DeleteCaptureDeviceSettings(const CStringW& displayName)
{
	if (!displayName.IsEmpty()) {
		AfxGetProfile().DeleteSection(CaptureDeviceSection(displayName));
	}
}

void WriteCaptureLastStatus(const CaptureDeviceSettings& cds, const CStringW& status)
{
	if (cds.displayName.IsEmpty() || cds.IsDefault()) {
		return;
	}

	CProfile& profile = AfxGetProfile();
	const CStringW section = CaptureDeviceSection(cds.displayName);

	CStringW storedName;
	if (profile.ReadString(section, L"DisplayName", storedName) && storedName == cds.displayName) {
		profile.WriteString(section, L"LastStatus", status);
	}
}

bool FillCaptureSettingsFromMediaType(const AM_MEDIA_TYPE* pmt, CaptureDeviceSettings& cds)
{
	if (!pmt || pmt->majortype != MEDIATYPE_Video) {
		return false;
	}

	const BITMAPINFOHEADER* pBIH = GetBitmapInfoHeader(pmt);
	if (!pBIH || pBIH->biWidth <= 0 || pBIH->biHeight == 0) {
		return false;
	}

	cds.subtype = pmt->subtype;
	cds.fourcc  = FourCCString(pmt);
	cds.width   = pBIH->biWidth;
	cds.height  = abs(pBIH->biHeight);

	REFERENCE_TIME atpf = 0;
	cds.frameInterval = ExtractAvgTimePerFrame(pmt, atpf) && atpf > 0 ? atpf : 0;

	return true;
}

bool IsStreamConfigPinConnected(IAMStreamConfig* pAMSC)
{
	if (CComQIPtr<IPin> pPin = pAMSC) {
		CComPtr<IPin> pPinTo;
		if (SUCCEEDED(pPin->ConnectedTo(&pPinTo)) && pPinTo) {
			return true;
		}
	}

	return false;
}

DWORD MakeHdr10ControlFlags()
{
	DXVA2_ExtendedFormat exfmt = {};

	exfmt.SampleFormat          = AMCONTROL_USED | AMCONTROL_COLORINFO_PRESENT;
	exfmt.NominalRange          = DXVA2_NominalRange_16_235;       // 2
	exfmt.VideoTransferMatrix   = MFVideoTransferMatrix_BT2020_10; // 4
	exfmt.VideoPrimaries        = MFVideoPrimaries_BT2020;         // 9
	exfmt.VideoTransferFunction = MFVideoTransFunc_2084;           // 15

	return exfmt.value;
}

CStringW DescribeMediaType(const AM_MEDIA_TYPE* pmt)
{
	if (!pmt) {
		return L"(null)";
	}

	CStringW str = FourCCString(pmt);
	if (str.IsEmpty()) {
		str = CStringFromGUID(pmt->subtype);
	}

	if (const BITMAPINFOHEADER* pBIH = GetBitmapInfoHeader(pmt)) {
		str.AppendFormat(L" %ldx%ld", pBIH->biWidth, pBIH->biHeight);
	}

	REFERENCE_TIME atpf = 0;
	if (ExtractAvgTimePerFrame(pmt, atpf) && atpf > 0) {
		str.AppendFormat(L" @%lld (%.3f fps)", atpf, 10000000.0 / atpf);
	}

	if (pmt->formattype == FORMAT_VideoInfo) {
		str.Append(L" VideoInfo");
	} else if (pmt->formattype == FORMAT_VideoInfo2) {
		str.Append(L" VideoInfo2");
	}

	return str;
}

HRESULT ApplyCaptureFormat(IAMStreamConfig* pAMSC, const CaptureDeviceSettings& cds, CStringW& why)
{
	why.Empty();

	if (!pAMSC) {
		why = L"no IAMStreamConfig";
		return E_POINTER;
	}
	if (!cds.bExplicit || cds.subtype == GUID_NULL || cds.width <= 0 || cds.height <= 0) {
		why = L"no explicit format configured";
		return E_INVALIDARG;
	}
	if (IsStreamConfigPinConnected(pAMSC)) {
		why = L"pin already connected";
		return VFW_E_ALREADY_CONNECTED;
	}

	// current format, used for the format type tie break and as the rollback target
	GUID       currentFormatType = GUID_NULL;
	CMediaType mtOriginal;
	bool       bHaveOriginal = false;
	{
		AM_MEDIA_TYPE* pcurmt = nullptr;
		if (SUCCEEDED(pAMSC->GetFormat(&pcurmt)) && pcurmt) {
			currentFormatType = pcurmt->formattype;
			mtOriginal        = *pcurmt;
			bHaveOriginal     = true;
			DeleteMediaType(pcurmt);
		}
	}

	int iCount = 0, iSize = 0;
	HRESULT hr = pAMSC->GetNumberOfCapabilities(&iCount, &iSize);
	if (FAILED(hr) || iSize != sizeof(VIDEO_STREAM_CONFIG_CAPS) || iCount <= 0) {
		why.Format(L"GetNumberOfCapabilities hr=0x%08x count=%d size=%d", hr, iCount, iSize);
		return FAILED(hr) ? hr : E_FAIL;
	}

	std::vector<Candidate> candidates;

	for (int i = 0; i < iCount; i++) {
		VIDEO_STREAM_CONFIG_CAPS caps = {};
		AM_MEDIA_TYPE* pmt = nullptr;
		if (FAILED(pAMSC->GetStreamCaps(i, &pmt, (BYTE*)&caps)) || !pmt) {
			continue;
		}

		Candidate cand;
		cand.index = i;
		cand.caps  = caps;
		cand.mt    = *pmt; // CMediaType copies the format block
		DeleteMediaType(pmt);

		const BITMAPINFOHEADER* pBIH = GetBitmapInfoHeader(&cand.mt);

		if (cand.mt.majortype != MEDIATYPE_Video || !pBIH
				|| (cand.mt.formattype != FORMAT_VideoInfo && cand.mt.formattype != FORMAT_VideoInfo2)) {
			continue;
		}

		// 1) subtype, with a FOURCC fallback for drivers that only fill biCompression
		if (cand.mt.subtype != cds.subtype) {
			if (cds.fourcc.IsEmpty() || FourCCString(&cand.mt) != cds.fourcc) {
				continue;
			}
		}

		// 2) frame size
		const LONG cw = pBIH->biWidth;
		const LONG ch = abs(pBIH->biHeight);

		if (cw == cds.width && ch == cds.height) {
			cand.sizeScore = 0;
		} else if (cds.width  >= caps.MinOutputSize.cx && cds.width  <= caps.MaxOutputSize.cx
				&& cds.height >= caps.MinOutputSize.cy && cds.height <= caps.MaxOutputSize.cy
				&& (caps.OutputGranularityX <= 1
					|| ((cds.width - caps.MinOutputSize.cx) % caps.OutputGranularityX) == 0
					|| (cds.width % caps.OutputGranularityX) == 0)
				&& (caps.OutputGranularityY <= 1
					|| ((cds.height - caps.MinOutputSize.cy) % caps.OutputGranularityY) == 0
					|| (cds.height % caps.OutputGranularityY) == 0)) {
			cand.sizeScore = 1;
		} else {
			continue;
		}

		// 3) frame rate
		if (cds.frameInterval <= 0) {
			cand.fpsScore = 0;
		} else {
			REFERENCE_TIME atpf = 0;
			ExtractAvgTimePerFrame(&cand.mt, atpf);

			const double rel = atpf > 0
				? fabs((double)(atpf - cds.frameInterval)) / (double)cds.frameInterval
				: 1.0;

			if (rel <= 0.0002) {
				cand.fpsScore = 0;                       // exact
			} else if (rel <= 0.005) {
				cand.fpsScore = 1;                       // 59.94 vs 60
			} else if (caps.MinFrameInterval > 0 && caps.MaxFrameInterval > 0
					&& cds.frameInterval >= caps.MinFrameInterval && cds.frameInterval <= caps.MaxFrameInterval) {
				cand.fpsScore = 2;                       // inside the advertised range
			} else {
				cand.fpsScore = 3;                       // keep the capability's own rate
			}
		}

		cand.fmtScore = (currentFormatType != GUID_NULL && cand.mt.formattype != currentFormatType) ? 1 : 0;

		candidates.push_back(std::move(cand));
	}

	if (candidates.empty()) {
		why.Format(L"no capability matches %s %dx%d (%d caps)", cds.fourcc.GetString(), cds.width, cds.height, iCount);
		return VFW_E_INVALIDMEDIATYPE;
	}

	std::stable_sort(candidates.begin(), candidates.end());

	const size_t maxTries = std::min<size_t>(candidates.size(), 4);
	bool bTouched = false;

	for (size_t i = 0; i < maxTries; i++) {
		CMediaType mt = candidates[i].mt;

		if (candidates[i].sizeScore == 1) {
			ApplyFrameSize(mt, cds.width, cds.height);
		}
		if (cds.frameInterval > 0 && candidates[i].fpsScore == 2) {
			SetAvgTimePerFrame(&mt, cds.frameInterval);
		}

		hr = pAMSC->SetFormat(&mt);

		CStringW line;
		line.Format(L"[cap %d %s] SetFormat=0x%08x", candidates[i].index, DescribeMediaType(&mt).GetString(), hr);

		if (SUCCEEDED(hr)) {
			// some KS proxies answer S_OK without actually switching
			AM_MEDIA_TYPE* pmtNow = nullptr;
			if (SUCCEEDED(pAMSC->GetFormat(&pmtNow)) && pmtNow) {
				const BITMAPINFOHEADER* pBIH = GetBitmapInfoHeader(pmtNow);
				const bool bOk = pmtNow->subtype == mt.subtype
								 && pBIH && pBIH->biWidth == cds.width && abs(pBIH->biHeight) == cds.height;
				line.AppendFormat(L" -> now %s", DescribeMediaType(pmtNow).GetString());
				DeleteMediaType(pmtNow);

				if (bOk) {
					why = line;
					CaptureDiag(L"CaptureFormat: %s", why.GetString());
					return S_OK;
				}

				hr = E_FAIL;
				bTouched = true; // the driver did take a format, just not the one we asked for
				line.Append(L" (not accepted)");
			} else {
				// cannot verify, trust SetFormat
				why = line + L" (unverified)";
				CaptureDiag(L"CaptureFormat: %s", why.GetString());
				return S_OK;
			}
		}

		if (!why.IsEmpty()) {
			why.Append(L"; ");
		}
		why.Append(line);
	}

	if (bTouched && bHaveOriginal) {
		// do not leave the device on a half applied format nobody asked for
		const HRESULT hrRestore = pAMSC->SetFormat(&mtOriginal);
		why.AppendFormat(L"; restore %s -> 0x%08x", DescribeMediaType(&mtOriginal).GetString(), hrRestore);
	}

	CaptureDiag(L"CaptureFormat: failed - %s", why.GetString());

	return FAILED(hr) ? hr : E_FAIL;
}

void CaptureDiag(LPCWSTR fmt, ...)
{
	va_list args;
	va_start(args, fmt);

	wchar_t buffer[1024] = {};
	const int len = _vsnwprintf_s(buffer, _TRUNCATE, fmt, args);
	va_end(args);

	if (len < 0) {
		buffer[_countof(buffer) - 1] = 0;
	}

	DLog(L"%s", buffer);

	CStringW str(buffer);
	str.Append(L"\n");
	OutputDebugStringW(str);
}
