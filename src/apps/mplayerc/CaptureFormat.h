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
// Per capture device video format settings.
//
// The settings are keyed by a hash of the device moniker display name and stored
// in the profile section "Capture\Devices\<hash>". The full display name is stored
// as a value and verified on load, so a hash collision simply reads as "no settings".
//
// Semantics:
//   bExplicit == false -> nothing new happens, MPC-BE behaves exactly like before.
//   bForceHDR == false -> the colorimetry tagging filter is never created.
//
struct CaptureDeviceSettings {
	CStringW       displayName;
	bool           bExplicit     = false; // apply subtype/width/height/frameInterval via IAMStreamConfig::SetFormat()
	GUID           subtype       = GUID_NULL;
	CStringW       fourcc;                // informative, also used as a fallback match
	int            width         = 0;
	int            height        = 0;     // always positive, the sign is taken from the enumerated capability
	REFERENCE_TIME frameInterval = 0;     // AvgTimePerFrame in 100ns units, 0 = keep the capability's own value
	bool           bForceHDR     = false; // tag the capture stream as BT.2020 / BT.2020 / ST 2084 / 16-235

	bool IsDefault() const {
		return !bExplicit && !bForceHDR;
	}
};

// Profile section name for a device, e.g. L"Capture\\Devices\\1A2B3C4D5E6F7081"
CStringW CaptureDeviceSection(const CStringW& displayName);

// Returns false when no (matching) section exists. cds is always reset first.
bool LoadCaptureDeviceSettings(const CStringW& displayName, CaptureDeviceSettings& cds);
void SaveCaptureDeviceSettings(const CaptureDeviceSettings& cds);
void DeleteCaptureDeviceSettings(const CStringW& displayName);
// Diagnostic breadcrumb, only written when the device already has a section.
void WriteCaptureLastStatus(const CaptureDeviceSettings& cds, const CStringW& status);

// Fills subtype/fourcc/width/height/frameInterval from a video media type.
bool FillCaptureSettingsFromMediaType(const AM_MEDIA_TYPE* pmt, CaptureDeviceSettings& cds);

// Picks the best matching capabilities for cds and applies them with SetFormat().
// The result is verified with GetFormat(); up to 4 ranked candidates are tried.
// 'why' receives a human readable trace for the log / LastStatus value.
HRESULT ApplyCaptureFormat(IAMStreamConfig* pAMSC, const CaptureDeviceSettings& cds, CStringW& why);

// true when the pin behind the IAMStreamConfig is already connected (SetFormat must not be used then).
bool IsStreamConfigPinConnected(IAMStreamConfig* pAMSC);

// BT.2020 matrix + BT.2020 primaries + SMPTE ST 2084 (PQ) + 16-235, marked as present.
DXVA2_ExtendedFormat MakeHdr10ExtendedFormat();

// "P010 3840x2160 @166833 VideoInfo" - for logging.
CStringW DescribeMediaType(const AM_MEDIA_TYPE* pmt);

// Visible in DebugView in release builds (DLog is a no-op there).
void CaptureDiag(LPCWSTR fmt, ...);
