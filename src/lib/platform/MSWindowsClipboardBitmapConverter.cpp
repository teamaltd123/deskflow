/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Symless Ltd.
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-FileCopyrightText: (C) 2004 Chris Schoeneman
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "platform/MSWindowsClipboardBitmapConverter.h"

#include "base/ClipboardBitmap.h"
#include "base/Log.h"

#include <string_view>

namespace {

// Peers and Windows apps produce many DIB variants (V4/V5 headers, bit fields,
// top-down rows, alpha, and the malformed DIBs sent by Deskflow <= 1.26 on macOS
// and X11). Only the canonical 24 bpp, bottom-up BI_RGB form goes on the wire or
// into CF_DIB; see ClipboardBitmap.h.
std::string canonicalise(std::string_view dib, const char *direction)
{
  const auto result = deskflow::clipboard::canonicalDibFromDib(dib);
  if (!result.error.empty()) {
    LOG_WARN("dropping %s clipboard image: %s", direction, result.error.c_str());
    return std::string();
  }
  if (result.repairedLegacy) {
    LOG_INFO("repaired malformed clipboard image from an older macOS/X11 peer");
  }
  if (result.flattenedAlpha) {
    LOG_DEBUG("flattened clipboard image transparency onto white");
  }
  return result.dib;
}

} // namespace

//
// MSWindowsClipboardBitmapConverter
//

IClipboard::Format MSWindowsClipboardBitmapConverter::getFormat() const
{
  return IClipboard::Format::Bitmap;
}

UINT MSWindowsClipboardBitmapConverter::getWin32Format() const
{
  return CF_DIB;
}

HANDLE
MSWindowsClipboardBitmapConverter::fromIClipboard(const std::string &data) const
{
  const std::string dib = canonicalise(data, "received");
  if (dib.empty()) {
    return nullptr;
  }

  // copy to memory handle
  HGLOBAL gData = GlobalAlloc(GMEM_MOVEABLE | GMEM_DDESHARE, dib.size());
  if (gData != nullptr) {
    // get a pointer to the allocated memory
    char *dst = (char *)GlobalLock(gData);
    if (dst != nullptr) {
      memcpy(dst, dib.data(), dib.size());
      GlobalUnlock(gData);
    } else {
      GlobalFree(gData);
      gData = nullptr;
    }
  }

  return gData;
}

std::string MSWindowsClipboardBitmapConverter::toIClipboard(HANDLE data) const
{
  // get datator
  LPVOID src = GlobalLock(data);
  if (src == nullptr) {
    return std::string();
  }
  const auto srcSize = static_cast<size_t>(GlobalSize(data));

  // 24/32 bpp images (any header version, bit fields, top-down) convert directly
  auto direct = deskflow::clipboard::canonicalDibFromDib(std::string_view(static_cast<const char *>(src), srcSize));
  if (direct.error.empty()) {
    GlobalUnlock(data);
    return std::move(direct.dib);
  }

  // anything else (palettes, 16 bpp, RLE) is rendered to 32 bpp by GDI first
  if (srcSize < sizeof(BITMAPINFOHEADER)) {
    GlobalUnlock(data);
    LOG_WARN("dropping sent clipboard image: %s", direct.error.c_str());
    return std::string();
  }
  const BITMAPINFO *bitmap = static_cast<const BITMAPINFO *>(src);
  LOG_DEBUG("converting clipboard image with GDI (%s)", direct.error.c_str());

  // create a destination DIB section
  LOG_INFO("convert image from: depth=%d comp=%d", bitmap->bmiHeader.biBitCount, bitmap->bmiHeader.biCompression);
  void *raw;
  BITMAPINFOHEADER info;
  LONG w = bitmap->bmiHeader.biWidth;
  LONG h = bitmap->bmiHeader.biHeight;
  const LONG absHeight = (h < 0) ? -h : h;
  info.biSize = sizeof(BITMAPINFOHEADER);
  info.biWidth = w;
  info.biHeight = h;
  info.biPlanes = 1;
  info.biBitCount = 32;
  info.biCompression = BI_RGB;
  info.biSizeImage = 0;
  info.biXPelsPerMeter = 1000;
  info.biYPelsPerMeter = 1000;
  info.biClrUsed = 0;
  info.biClrImportant = 0;
  HDC dc = GetDC(nullptr);
  HBITMAP dst = CreateDIBSection(dc, (BITMAPINFO *)&info, DIB_RGB_COLORS, &raw, nullptr, 0);
  if (dst == nullptr || raw == nullptr) {
    LOG_WARN("failed to allocate destination bitmap for clipboard image");
    ReleaseDC(nullptr, dc);
    GlobalUnlock(data);
    return std::string();
  }

  // find the start of the pixel data
  const char *srcBits = (const char *)bitmap + bitmap->bmiHeader.biSize;
  if (bitmap->bmiHeader.biBitCount >= 16) {
    // bit masks follow a BITMAPINFOHEADER; V4/V5 headers already contain them
    if (bitmap->bmiHeader.biCompression == BI_BITFIELDS && bitmap->bmiHeader.biSize == sizeof(BITMAPINFOHEADER) &&
        (bitmap->bmiHeader.biBitCount == 16 || bitmap->bmiHeader.biBitCount == 32)) {
      srcBits += 3 * sizeof(DWORD);
    }
  } else if (bitmap->bmiHeader.biClrUsed != 0) {
    srcBits += bitmap->bmiHeader.biClrUsed * sizeof(RGBQUAD);
  } else {
    // http://msdn.microsoft.com/en-us/library/ke55d167(VS.80).aspx
    srcBits += (1i64 << bitmap->bmiHeader.biBitCount) * sizeof(RGBQUAD);
  }

  // copy source image to destination image
  HDC dstDC = CreateCompatibleDC(dc);
  HGDIOBJ oldBitmap = SelectObject(dstDC, dst);
  SetDIBitsToDevice(dstDC, 0, 0, w, absHeight, 0, 0, 0, absHeight, srcBits, bitmap, DIB_RGB_COLORS);
  SelectObject(dstDC, oldBitmap);
  DeleteDC(dstDC);
  GdiFlush();

  // extract data
  std::string image((const char *)&info, info.biSize);
  image.append((const char *)raw, static_cast<size_t>(4) * static_cast<size_t>(w) * static_cast<size_t>(absHeight));

  // clean up GDI
  DeleteObject(dst);
  ReleaseDC(nullptr, dc);

  // release handle
  GlobalUnlock(data);

  return canonicalise(image, "sent");
}
