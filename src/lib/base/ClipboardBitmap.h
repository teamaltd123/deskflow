/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <string>
#include <string_view>

namespace deskflow::clipboard {

//! Outcome of converting an image to the clipboard wire format
struct BitmapResult
{
  std::string dib;             //!< canonical DIB; empty when the input was rejected
  std::string error;           //!< why the input was rejected; empty on success
  bool repairedLegacy = false; //!< input was the malformed DIB sent by Deskflow <= 1.26 (macOS/X11)
  bool flattenedAlpha = false; //!< input had real transparency that was composited over white
};

/*!
Convert a DIB (a BMP without its 14 byte file header) to the canonical wire
format for IClipboard::Format::Bitmap:

- 40 byte BITMAPINFOHEADER, biCompression = BI_RGB, biBitCount = 24
- positive biHeight (bottom-up rows); some Windows apps (e.g. Word, WordPad)
  cannot paste top-down DIBs
- rows padded to 4 bytes, pixel data immediately after the header
- transparency composited over white, because most consumers of CF_DIB and
  BI_RGB bitmaps ignore alpha and would otherwise show hidden colours or black

Accepted input: BITMAPINFOHEADER/V2/V3/V4/V5 headers, BI_RGB, BI_BITFIELDS and
BI_ALPHABITFIELDS, 24 or 32 bpp, top-down or bottom-up, optional colour table,
trailing data (e.g. an embedded ICC profile). Also repairs the malformed DIBs
sent by Deskflow <= 1.26 on macOS and X11, which declare a V4/V5 header but
carry only 40 header bytes before the pixels.

Never reads outside the input; rejects anything it cannot interpret.
*/
BitmapResult canonicalDibFromDib(std::string_view dib);

//! As canonicalDibFromDib(), for a complete .bmp file; honours its pixel offset
BitmapResult canonicalDibFromBmpFile(std::string_view bmp);

//! Wrap a canonical DIB in a 14 byte BITMAPFILEHEADER (pixel offset 54)
std::string bmpFileFromCanonicalDib(std::string_view dib);

} // namespace deskflow::clipboard
