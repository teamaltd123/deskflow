/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ClipboardBitmapTests.h"

#include "base/ClipboardBitmap.h"

#include <cstdint>
#include <string>
#include <vector>

using namespace deskflow::clipboard;

namespace {

struct Pixel
{
  uint8_t r;
  uint8_t g;
  uint8_t b;
  uint8_t a;
};

// 2x2 image, top row first: opaque red, 50% green, transparent blue, opaque white
const std::vector<Pixel> kImage = {{255, 0, 0, 255}, {0, 255, 0, 128}, {0, 0, 255, 0}, {255, 255, 255, 255}};

void put16(std::string &s, size_t offset, uint16_t value)
{
  s[offset] = static_cast<char>(value & 0xff);
  s[offset + 1] = static_cast<char>(value >> 8);
}

void put32(std::string &s, size_t offset, uint32_t value)
{
  for (size_t i = 0; i < 4; ++i) {
    s[offset + i] = static_cast<char>((value >> (8 * i)) & 0xff);
  }
}

uint32_t get32(const std::string &s, size_t offset)
{
  uint32_t value = 0;
  for (size_t i = 0; i < 4; ++i) {
    value |= static_cast<uint32_t>(static_cast<uint8_t>(s[offset + i])) << (8 * i);
  }
  return value;
}

uint8_t overWhite(uint8_t colour, uint8_t alpha)
{
  return static_cast<uint8_t>((colour * alpha + 255 * (255 - alpha) + 127) / 255);
}

// header + 32 bpp BGRA pixels; masks is written after the header when non-empty
std::string makeDib32(
    uint32_t headerSize, uint32_t compression, bool topDown, const std::vector<uint32_t> &masks, bool alphaInPixels,
    size_t declaredHeaderSize = 0
)
{
  const size_t pixelOffset = headerSize + (headerSize == 40 ? masks.size() * 4 : 0);
  std::string dib(pixelOffset + kImage.size() * 4, '\0');
  put32(dib, 0, declaredHeaderSize ? static_cast<uint32_t>(declaredHeaderSize) : headerSize);
  put32(dib, 4, 2);
  put32(dib, 8, static_cast<uint32_t>(topDown ? -2 : 2));
  put16(dib, 12, 1);
  put16(dib, 14, 32);
  put32(dib, 16, compression);
  for (size_t i = 0; i < masks.size(); ++i) {
    put32(dib, 40 + i * 4, masks[i]);
  }
  for (size_t row = 0; row < 2; ++row) {
    const size_t imageRow = topDown ? row : 1 - row;
    for (size_t x = 0; x < 2; ++x) {
      const Pixel &p = kImage[imageRow * 2 + x];
      const size_t o = pixelOffset + (row * 2 + x) * 4;
      dib[o] = static_cast<char>(p.b);
      dib[o + 1] = static_cast<char>(p.g);
      dib[o + 2] = static_cast<char>(p.r);
      dib[o + 3] = static_cast<char>(alphaInPixels ? p.a : 0);
    }
  }
  return dib;
}

// verify the canonical contract and the pixels (alpha composited over white when used)
void verifyCanonical(const BitmapResult &result, bool alphaUsed)
{
  QVERIFY2(result.error.empty(), result.error.c_str());
  const std::string &dib = result.dib;
  QCOMPARE(dib.size(), size_t(40 + 2 * 8)); // 2x2 at 24 bpp: 6 bytes + 2 padding per row
  QCOMPARE(get32(dib, 0), uint32_t(40));
  QCOMPARE(get32(dib, 4), uint32_t(2));
  QCOMPARE(get32(dib, 8), uint32_t(2)); // positive height: bottom-up
  QCOMPARE(static_cast<uint8_t>(dib[14]), uint8_t(24));
  QCOMPARE(get32(dib, 16), uint32_t(0)); // BI_RGB
  QCOMPARE(get32(dib, 20), uint32_t(16));
  for (size_t y = 0; y < 2; ++y) {
    const size_t row = 40 + (1 - y) * 8; // bottom-up storage
    for (size_t x = 0; x < 2; ++x) {
      const Pixel &p = kImage[y * 2 + x];
      const uint8_t a = alphaUsed ? p.a : 255;
      QCOMPARE(static_cast<uint8_t>(dib[row + x * 3]), overWhite(p.b, a));
      QCOMPARE(static_cast<uint8_t>(dib[row + x * 3 + 1]), overWhite(p.g, a));
      QCOMPARE(static_cast<uint8_t>(dib[row + x * 3 + 2]), overWhite(p.r, a));
    }
  }
}

const std::vector<uint32_t> kBgraMasks = {0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000};

} // namespace

void ClipboardBitmapTests::repairsLegacyMacDib()
{
  // Deskflow <= 1.26 on macOS: V5 declared, 40 header bytes, BI_BITFIELDS, masks dropped
  const auto dib = makeDib32(40, 3, true, {}, true, 124);
  const auto result = canonicalDibFromDib(dib);
  verifyCanonical(result, true);
  QVERIFY(result.repairedLegacy);
  QVERIFY(result.flattenedAlpha);
}

void ClipboardBitmapTests::repairsLegacyX11V4Dib()
{
  const auto result = canonicalDibFromDib(makeDib32(40, 3, false, {}, true, 108));
  verifyCanonical(result, true);
  QVERIFY(result.repairedLegacy);
}

void ClipboardBitmapTests::convertsTopDownV5WithAlpha()
{
  // what macOS itself puts on the pasteboard (and Deskflow master sends)
  std::string dib = makeDib32(124, 3, true, {}, true);
  for (size_t i = 0; i < kBgraMasks.size(); ++i) {
    put32(dib, 40 + i * 4, kBgraMasks[i]);
  }
  const auto result = canonicalDibFromDib(dib);
  verifyCanonical(result, true);
  QVERIFY(!result.repairedLegacy);
}

void ClipboardBitmapTests::ignoresAllZeroAlpha()
{
  // GDI screen captures leave the spare byte at zero: the image is opaque
  const auto result = canonicalDibFromDib(makeDib32(40, 0, false, {}, false));
  verifyCanonical(result, false);
  QVERIFY(!result.flattenedAlpha);
}

void ClipboardBitmapTests::usesAlphaInBiRgb()
{
  // Chrome/Electron put real alpha in the spare byte of BI_RGB images
  verifyCanonical(canonicalDibFromDib(makeDib32(40, 0, false, {}, true)), true);
}

void ClipboardBitmapTests::readsMasksAfterInfoHeader()
{
  verifyCanonical(canonicalDibFromDib(makeDib32(40, 3, false, {0x00ff0000, 0x0000ff00, 0x000000ff}, true)), true);
}

void ClipboardBitmapTests::skipsColourTable()
{
  // 32 bpp BI_RGB with an optional two entry colour table before the pixels
  std::string dib = makeDib32(40, 0, false, {}, false);
  put32(dib, 32, 2);
  dib.insert(40, std::string(8, '\x7f'));
  verifyCanonical(canonicalDibFromDib(dib), false);
}

void ClipboardBitmapTests::keepsCanonicalDib()
{
  const auto first = canonicalDibFromDib(makeDib32(124, 3, true, kBgraMasks, true));
  QVERIFY2(first.error.empty(), first.error.c_str());
  const auto second = canonicalDibFromDib(first.dib);
  QVERIFY2(second.error.empty(), second.error.c_str());
  QCOMPARE(second.dib, first.dib);
  QVERIFY(!second.flattenedAlpha);
}

void ClipboardBitmapTests::honoursBmpFilePixelOffset()
{
  // a .bmp file whose pixels start after a gap, as some encoders write them
  const std::string dib = makeDib32(40, 0, false, {}, false);
  std::string bmp(14, '\0');
  bmp[0] = 'B';
  bmp[1] = 'M';
  put32(bmp, 10, 14 + 40 + 6);
  bmp += dib.substr(0, 40) + std::string(6, '\x55') + dib.substr(40);
  put32(bmp, 2, static_cast<uint32_t>(bmp.size()));
  verifyCanonical(canonicalDibFromBmpFile(bmp), false);
}

void ClipboardBitmapTests::wrapsCanonicalDibAsBmpFile()
{
  const auto result = canonicalDibFromDib(makeDib32(40, 0, false, {}, false));
  const auto bmp = bmpFileFromCanonicalDib(result.dib);
  QCOMPARE(bmp.substr(0, 2), std::string("BM"));
  QCOMPARE(get32(bmp, 2), uint32_t(bmp.size()));
  QCOMPARE(get32(bmp, 10), uint32_t(54));
  QCOMPARE(bmp.substr(14), result.dib);
  QCOMPARE(canonicalDibFromBmpFile(bmp).dib, result.dib);
}

void ClipboardBitmapTests::rejectsMalformedInput()
{
  const std::string good = makeDib32(40, 0, false, {}, false);
  std::vector<std::string> bad;
  bad.push_back(std::string());
  bad.push_back(good.substr(0, 39));
  bad.push_back(good.substr(0, good.size() - 1)); // pixels truncated
  std::string s = good;
  put32(s, 0, 64); // unsupported header size
  bad.push_back(s);
  s = good;
  put32(s, 8, 0); // zero height
  bad.push_back(s);
  s = good;
  put32(s, 4, 0x7fffffff); // absurd width
  bad.push_back(s);
  s = good;
  put16(s, 14, 8); // palette images are left to the platform converter
  bad.push_back(s);
  bad.push_back(makeDib32(40, 3, false, {0x00ff0000, 0x00ff0000, 0x000000ff}, false)); // overlapping masks
  bad.push_back(makeDib32(40, 3, false, {0x00ff00ff, 0x0000ff00, 0x0000000f}, false)); // non-contiguous mask
  for (const auto &input : bad) {
    const auto result = canonicalDibFromDib(input);
    QVERIFY(!result.error.empty());
    QVERIFY(result.dib.empty());
  }
  QVERIFY(!canonicalDibFromBmpFile("BM").error.empty());
}

QTEST_MAIN(ClipboardBitmapTests)
