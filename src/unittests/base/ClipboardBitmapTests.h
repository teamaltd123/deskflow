/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include <QTest>

class ClipboardBitmapTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void repairsLegacyMacDib();
  void repairsLegacyX11V4Dib();
  void convertsTopDownV5WithAlpha();
  void ignoresAllZeroAlpha();
  void usesAlphaInBiRgb();
  void readsMasksAfterInfoHeader();
  void skipsColourTable();
  void keepsCanonicalDib();
  void honoursBmpFilePixelOffset();
  void wrapsCanonicalDibAsBmpFile();
  void rejectsMalformedInput();
};
