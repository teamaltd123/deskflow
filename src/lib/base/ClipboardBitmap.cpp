/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "base/ClipboardBitmap.h"

#include <cstdint>
#include <cstring>
#include <optional>

namespace deskflow::clipboard {

namespace {

constexpr size_t kFileHeaderSize = 14;
constexpr size_t kInfoHeaderSize = 40;
constexpr uint32_t kBiRgb = 0;
constexpr uint32_t kBiBitfields = 3;
constexpr uint32_t kBiAlphaBitfields = 6;
constexpr int64_t kMaxDimension = 1 << 16;
constexpr uint64_t kMaxPixels = uint64_t{1} << 28;
constexpr uint32_t kMaxColourTable = 256;
constexpr int32_t kDefaultPelsPerMeter = 2835; // 72 dpi

uint16_t readU16(const char *p)
{
  return static_cast<uint16_t>(static_cast<uint8_t>(p[0]) | static_cast<uint8_t>(p[1]) << 8);
}

uint32_t readU32(const char *p)
{
  return static_cast<uint32_t>(static_cast<uint8_t>(p[0])) | static_cast<uint32_t>(static_cast<uint8_t>(p[1])) << 8 |
         static_cast<uint32_t>(static_cast<uint8_t>(p[2])) << 16 |
         static_cast<uint32_t>(static_cast<uint8_t>(p[3])) << 24;
}

void writeU16(char *p, uint16_t v)
{
  p[0] = static_cast<char>(v & 0xff);
  p[1] = static_cast<char>(v >> 8);
}

void writeU32(char *p, uint32_t v)
{
  for (int i = 0; i < 4; ++i) {
    p[i] = static_cast<char>((v >> (8 * i)) & 0xff);
  }
}

struct Channel
{
  uint32_t mask = 0;
  int shift = 0;
  uint32_t max = 0;

  uint8_t read(uint32_t pixel) const
  {
    if (mask == 0) {
      return 0;
    }
    const uint64_t value = (pixel & mask) >> shift;
    return static_cast<uint8_t>((value * 255 + max / 2) / max);
  }
};

bool makeChannel(uint32_t mask, Channel &channel)
{
  channel = {};
  if (mask == 0) {
    return true;
  }
  int shift = 0;
  while (((mask >> shift) & 1u) == 0) {
    ++shift;
  }
  const uint64_t value = mask >> shift;
  if ((value & (value + 1)) != 0) {
    return false; // mask bits must be contiguous
  }
  channel.mask = mask;
  channel.shift = shift;
  channel.max = static_cast<uint32_t>(value);
  return true;
}

struct Layout
{
  uint32_t width = 0;
  uint32_t height = 0;
  bool topDown = false;
  uint16_t bitCount = 0;
  size_t pixelOffset = 0;
  size_t stride = 0;
  Channel red;
  Channel green;
  Channel blue;
  Channel alpha;
  int32_t xPelsPerMeter = kDefaultPelsPerMeter;
  int32_t yPelsPerMeter = kDefaultPelsPerMeter;
  bool legacy = false;
};

std::optional<Layout> parseDib(std::string_view dib, std::optional<size_t> filePixelOffset, std::string &error)
{
  if (dib.size() < kInfoHeaderSize) {
    error = "too small for a BITMAPINFOHEADER";
    return std::nullopt;
  }

  const char *p = dib.data();
  const uint32_t headerSize = readU32(p);
  if (headerSize != 40 && headerSize != 52 && headerSize != 56 && headerSize != 108 && headerSize != 124) {
    error = "unsupported header size " + std::to_string(headerSize);
    return std::nullopt;
  }

  const int64_t width = static_cast<int32_t>(readU32(p + 4));
  const int64_t rawHeight = static_cast<int32_t>(readU32(p + 8));
  const uint16_t planes = readU16(p + 12);
  const uint16_t bitCount = readU16(p + 14);
  const uint32_t compression = readU32(p + 16);
  const uint32_t coloursUsed = readU32(p + 32);

  if (planes != 1) {
    error = "unsupported plane count " + std::to_string(planes);
    return std::nullopt;
  }
  if (width <= 0 || width > kMaxDimension || rawHeight == 0 || rawHeight > kMaxDimension ||
      rawHeight < -kMaxDimension) {
    error = "unsupported dimensions " + std::to_string(width) + "x" + std::to_string(rawHeight);
    return std::nullopt;
  }
  const uint64_t height = rawHeight < 0 ? static_cast<uint64_t>(-rawHeight) : static_cast<uint64_t>(rawHeight);
  if (static_cast<uint64_t>(width) * height > kMaxPixels) {
    error = "image too large";
    return std::nullopt;
  }
  if (bitCount != 24 && bitCount != 32) {
    error = "unsupported bit depth " + std::to_string(bitCount);
    return std::nullopt;
  }

  Layout layout;
  layout.width = static_cast<uint32_t>(width);
  layout.height = static_cast<uint32_t>(height);
  layout.topDown = rawHeight < 0;
  layout.bitCount = bitCount;
  layout.stride = static_cast<size_t>((static_cast<uint64_t>(width) * bitCount + 31) / 32 * 4);
  const size_t imageBytes = layout.stride * layout.height;

  const int32_t xPels = static_cast<int32_t>(readU32(p + 24));
  const int32_t yPels = static_cast<int32_t>(readU32(p + 28));
  if (xPels > 0 && yPels > 0) {
    layout.xPelsPerMeter = xPels;
    layout.yPelsPerMeter = yPels;
  }

  uint32_t redMask = 0x00ff0000;
  uint32_t greenMask = 0x0000ff00;
  uint32_t blueMask = 0x000000ff;
  uint32_t alphaMask = 0;

  // Deskflow <= 1.26 on macOS and X11 cut V4/V5 headers down to 40 bytes without
  // updating biSize, dropping the bit masks. A genuine V4/V5 DIB can never be this
  // short, so an exact size match identifies the malformed payload unambiguously.
  if (!filePixelOffset && headerSize > kInfoHeaderSize && dib.size() == kInfoHeaderSize + imageBytes) {
    layout.legacy = true;
    layout.pixelOffset = kInfoHeaderSize;
    if (bitCount == 32) {
      alphaMask = 0xff000000; // macOS sends BGRA with straight alpha
    }
  } else {
    if (headerSize > dib.size()) {
      error = "header truncated";
      return std::nullopt;
    }
    size_t offset = headerSize;
    if (compression == kBiRgb) {
      if (bitCount == 32) {
        // Officially unused, but Chrome/Electron put real alpha here; unused
        // padding is all zero, which convert() detects and ignores.
        alphaMask = 0xff000000;
      }
    } else if ((compression == kBiBitfields || compression == kBiAlphaBitfields) && bitCount == 32) {
      const size_t maskCount = compression == kBiAlphaBitfields ? 4 : 3;
      bool hasAlphaMask = headerSize >= 56; // V3 and later carry an alpha mask field
      if (headerSize == kInfoHeaderSize) {
        if (dib.size() < kInfoHeaderSize + maskCount * 4) {
          error = "bit masks truncated";
          return std::nullopt;
        }
        offset += maskCount * 4; // masks follow a BITMAPINFOHEADER
        hasAlphaMask = maskCount == 4;
      }
      redMask = readU32(p + 40);
      greenMask = readU32(p + 44);
      blueMask = readU32(p + 48);
      alphaMask = hasAlphaMask ? readU32(p + 52) : 0;
      if (alphaMask == 0 && ((redMask | greenMask | blueMask) & 0xff000000) == 0) {
        // Same as BI_RGB: the spare byte often holds real alpha (e.g. CF_DIB that
        // Windows synthesises from an alpha CF_DIBV5); all-zero padding is ignored.
        alphaMask = 0xff000000;
      }
    } else {
      error = "unsupported compression " + std::to_string(compression) + " at " + std::to_string(bitCount) + " bpp";
      return std::nullopt;
    }

    if (coloursUsed > kMaxColourTable) {
      error = "unsupported colour table size " + std::to_string(coloursUsed);
      return std::nullopt;
    }
    offset += static_cast<size_t>(coloursUsed) * 4; // optional palette on 24/32 bpp images

    if (filePixelOffset) {
      if (*filePixelOffset < headerSize || *filePixelOffset > dib.size()) {
        error = "pixel offset out of range";
        return std::nullopt;
      }
      offset = *filePixelOffset; // a .bmp file states where its pixels start
    }
    layout.pixelOffset = offset;
    if (layout.pixelOffset > dib.size() || dib.size() - layout.pixelOffset < imageBytes) {
      error = "pixel data truncated";
      return std::nullopt;
    }
  }

  if (!makeChannel(redMask, layout.red) || !makeChannel(greenMask, layout.green) ||
      !makeChannel(blueMask, layout.blue) || !makeChannel(alphaMask, layout.alpha)) {
    error = "non-contiguous bit mask";
    return std::nullopt;
  }
  if (redMask == 0 || greenMask == 0 || blueMask == 0 ||
      ((redMask & greenMask) | (redMask & blueMask) | (greenMask & blueMask) |
       (alphaMask & (redMask | greenMask | blueMask))) != 0) {
    error = "invalid bit masks";
    return std::nullopt;
  }
  return layout;
}

uint32_t readPixel(const char *p, uint16_t bitCount)
{
  if (bitCount == 32) {
    return readU32(p);
  }
  return static_cast<uint32_t>(static_cast<uint8_t>(p[0])) | static_cast<uint32_t>(static_cast<uint8_t>(p[1])) << 8 |
         static_cast<uint32_t>(static_cast<uint8_t>(p[2])) << 16;
}

uint8_t overWhite(uint8_t colour, uint8_t alpha)
{
  return static_cast<uint8_t>((colour * alpha + 255 * (255 - alpha) + 127) / 255);
}

BitmapResult convert(std::string_view dib, const Layout &layout)
{
  BitmapResult result;
  result.repairedLegacy = layout.legacy;

  const char *pixels = dib.data() + layout.pixelOffset;
  const size_t bytesPerPixel = layout.bitCount / 8;

  // An alpha channel that is zero everywhere is padding (e.g. GDI screen
  // captures), not a fully transparent image.
  bool useAlpha = false;
  if (layout.alpha.mask != 0) {
    for (uint32_t y = 0; y < layout.height && !useAlpha; ++y) {
      const char *row = pixels + y * layout.stride;
      for (uint32_t x = 0; x < layout.width; ++x) {
        if (layout.alpha.read(readPixel(row + x * bytesPerPixel, layout.bitCount)) != 0) {
          useAlpha = true;
          break;
        }
      }
    }
  }

  const bool plainBgr = layout.bitCount == 24 && layout.red.mask == 0x00ff0000 && layout.green.mask == 0x0000ff00 &&
                        layout.blue.mask == 0x000000ff;
  const size_t outStride = (static_cast<size_t>(layout.width) * 3 + 3) & ~static_cast<size_t>(3);

  std::string out(kInfoHeaderSize + outStride * layout.height, '\0');
  char *h = out.data();
  writeU32(h, static_cast<uint32_t>(kInfoHeaderSize));
  writeU32(h + 4, layout.width);
  writeU32(h + 8, layout.height);
  writeU16(h + 12, 1);
  writeU16(h + 14, 24);
  writeU32(h + 16, kBiRgb);
  writeU32(h + 20, static_cast<uint32_t>(outStride * layout.height));
  writeU32(h + 24, static_cast<uint32_t>(layout.xPelsPerMeter));
  writeU32(h + 28, static_cast<uint32_t>(layout.yPelsPerMeter));

  for (uint32_t outRow = 0; outRow < layout.height; ++outRow) {
    // output rows are bottom-up; top-down input stores the top row first
    const uint32_t srcRow = layout.topDown ? layout.height - 1 - outRow : outRow;
    const char *src = pixels + srcRow * layout.stride;
    char *dst = out.data() + kInfoHeaderSize + outRow * outStride;
    if (plainBgr) {
      std::memcpy(dst, src, static_cast<size_t>(layout.width) * 3);
      continue;
    }
    for (uint32_t x = 0; x < layout.width; ++x) {
      const uint32_t pixel = readPixel(src + x * bytesPerPixel, layout.bitCount);
      uint8_t r = layout.red.read(pixel);
      uint8_t g = layout.green.read(pixel);
      uint8_t b = layout.blue.read(pixel);
      if (useAlpha) {
        const uint8_t a = layout.alpha.read(pixel);
        if (a != 255) {
          result.flattenedAlpha = true;
          r = overWhite(r, a);
          g = overWhite(g, a);
          b = overWhite(b, a);
        }
      }
      dst[x * 3] = static_cast<char>(b);
      dst[x * 3 + 1] = static_cast<char>(g);
      dst[x * 3 + 2] = static_cast<char>(r);
    }
  }

  result.dib = std::move(out);
  return result;
}

} // namespace

BitmapResult canonicalDibFromDib(std::string_view dib)
{
  BitmapResult result;
  const auto layout = parseDib(dib, std::nullopt, result.error);
  if (!layout) {
    return result;
  }
  return convert(dib, *layout);
}

BitmapResult canonicalDibFromBmpFile(std::string_view bmp)
{
  BitmapResult result;
  if (bmp.size() < kFileHeaderSize + kInfoHeaderSize || bmp[0] != 'B' || bmp[1] != 'M') {
    result.error = "not a bmp file";
    return result;
  }
  const size_t fileOffset = readU32(bmp.data() + 10);
  if (fileOffset < kFileHeaderSize) {
    result.error = "pixel offset out of range";
    return result;
  }
  const auto dib = bmp.substr(kFileHeaderSize);
  const auto layout = parseDib(dib, fileOffset - kFileHeaderSize, result.error);
  if (!layout) {
    return result;
  }
  return convert(dib, *layout);
}

std::string bmpFileFromCanonicalDib(std::string_view dib)
{
  std::string bmp(kFileHeaderSize, '\0');
  bmp[0] = 'B';
  bmp[1] = 'M';
  writeU32(bmp.data() + 2, static_cast<uint32_t>(kFileHeaderSize + dib.size()));
  writeU32(bmp.data() + 10, static_cast<uint32_t>(kFileHeaderSize + kInfoHeaderSize));
  bmp.append(dib);
  return bmp;
}

} // namespace deskflow::clipboard
