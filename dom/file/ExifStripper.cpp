/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ExifStripper.h"

#include "mozilla/Preferences.h"
#include "mozilla/StaticPrefs_privacy.h"
#include "mozilla/dom/BlobImpl.h"
#include "mozilla/dom/BrowserChild.h"
#include "mozilla/dom/File.h"
#include "mozilla/dom/HTMLInputElement.h"
#include "mozilla/dom/UnionTypes.h"
#include "mozilla/glean/DomFileMetrics.h"
#include "mozilla/mozalloc.h"
#include "nsIInputStream.h"
#include "nsIObserver.h"
#include "nsNetUtil.h"
#include "nsProxyRelease.h"
#include "nsString.h"
#include "nsThreadUtils.h"
#include "zlib.h"

namespace mozilla::dom {

// ---------------------------------------------------------------------------
// JPEG stripping
// ---------------------------------------------------------------------------

static constexpr uint8_t kJpegSoi2 = 0xD8;
static constexpr uint8_t kJpegEoi2 = 0xD9;
static constexpr uint8_t kJpegApp1 = 0xE1;

// "Exif\0\0" — the APP1 payload identifier for EXIF data
static constexpr uint8_t kExifHeader[] = {'E', 'x', 'i', 'f', 0x00, 0x00};

static bool JpegHasExif(Span<const uint8_t> aInput) {
  if (aInput.Length() < 2 || aInput[0] != 0xFF || aInput[1] != kJpegSoi2) {
    return false;
  }

  size_t pos = 2;
  while (pos + 3 < aInput.Length()) {
    if (aInput[pos] != 0xFF) {
      break;
    }
    uint8_t marker = aInput[pos + 1];
    if (marker == kJpegEoi2) {
      break;
    }
    if (marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7)) {
      pos += 2;
      continue;
    }
    if (pos + 3 >= aInput.Length()) {
      break;
    }
    uint16_t segLen =
        (uint16_t(aInput[pos + 2]) << 8) | uint16_t(aInput[pos + 3]);
    if (segLen < 2) {
      break;
    }
    if (marker == kJpegApp1 && segLen >= 2 + sizeof(kExifHeader)) {
      size_t dataStart = pos + 4;
      if (dataStart + sizeof(kExifHeader) <= aInput.Length() &&
          memcmp(aInput.data() + dataStart, kExifHeader, sizeof(kExifHeader)) ==
              0) {
        return true;
      }
    }
    pos += 2 + segLen;
  }
  return false;
}

static Maybe<nsTArray<uint8_t>> StripExifFromJpeg(Span<const uint8_t> aInput) {
  if (!JpegHasExif(aInput)) {
    return Nothing();
  }

  glean::exif_strip::exif_detected.Get("jpeg"_ns).Add(1);

  nsTArray<uint8_t> out;
  // Copy SOI
  out.AppendElements(aInput.data(), 2);

  size_t pos = 2;
  while (pos < aInput.Length()) {
    if (aInput[pos] != 0xFF) {
      // Unexpected byte — copy remainder verbatim.
      out.AppendElements(aInput.data() + pos, aInput.Length() - pos);
      break;
    }
    if (pos + 1 >= aInput.Length()) {
      break;
    }
    uint8_t marker = aInput[pos + 1];

    // EOI: copy and stop.
    if (marker == kJpegEoi2) {
      out.AppendElements(aInput.data() + pos, 2);
      break;
    }

    // Standalone markers (RST0-RST7, SOI) have no length field.
    if (marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7)) {
      out.AppendElements(aInput.data() + pos, 2);
      pos += 2;
      continue;
    }

    if (pos + 3 >= aInput.Length()) {
      break;
    }
    uint16_t segLen =
        (uint16_t(aInput[pos + 2]) << 8) | uint16_t(aInput[pos + 3]);
    if (segLen < 2) {
      // Malformed segment — abort.
      break;
    }
    size_t totalSegSize = 2 + segLen;  // marker(2) + length(2) + data(segLen-2)
    if (pos + totalSegSize > aInput.Length()) {
      // Segment extends past end — abort.
      break;
    }

    bool isExifApp1 = (marker == kJpegApp1) &&
                      (segLen >= 2 + (uint16_t)sizeof(kExifHeader)) &&
                      (memcmp(aInput.data() + pos + 4, kExifHeader,
                              sizeof(kExifHeader)) == 0);

    if (!isExifApp1) {
      out.AppendElements(aInput.data() + pos, totalSegSize);
    }
    pos += totalSegSize;
  }

  glean::exif_strip::exif_stripped.Get("jpeg"_ns).Add(1);
  return Some(std::move(out));
}

// ---------------------------------------------------------------------------
// PNG stripping
// ---------------------------------------------------------------------------

static constexpr uint8_t kPngSignature[] = {0x89, 0x50, 0x4E, 0x47,
                                            0x0D, 0x0A, 0x1A, 0x0A};

// Chunk types we strip
static constexpr uint8_t kChunkEXIf[] = {'e', 'X', 'I', 'f'};

// Known metadata keywords in tEXt/iTXt/zTXt chunks
static const char* const kMetadataKeywords[] = {
    "Raw profile type exif",
    "Raw profile type APP1",
    "XML:com.adobe.xmp",
    nullptr,
};

static bool IsMetadataTextChunk(Span<const uint8_t> aChunkType,
                                Span<const uint8_t> aData) {
  bool isText = (memcmp(aChunkType.data(), "tEXt", 4) == 0 ||
                 memcmp(aChunkType.data(), "iTXt", 4) == 0 ||
                 memcmp(aChunkType.data(), "zTXt", 4) == 0);
  if (!isText) {
    return false;
  }
  for (size_t i = 0; kMetadataKeywords[i]; ++i) {
    const char* kw = kMetadataKeywords[i];
    size_t kwLen = strlen(kw);
    if (aData.Length() >= kwLen && memcmp(aData.data(), kw, kwLen) == 0) {
      return true;
    }
  }
  return false;
}

static bool PngHasExif(Span<const uint8_t> aInput) {
  if (aInput.Length() < 8 || memcmp(aInput.data(), kPngSignature, 8) != 0) {
    return false;
  }

  size_t pos = 8;
  while (pos + 12 <= aInput.Length()) {
    uint32_t dataLen =
        (uint32_t(aInput[pos]) << 24) | (uint32_t(aInput[pos + 1]) << 16) |
        (uint32_t(aInput[pos + 2]) << 8) | uint32_t(aInput[pos + 3]);
    Span<const uint8_t> chunkType = aInput.subspan(pos + 4, 4);
    size_t totalChunk = size_t(12) + size_t(dataLen);
    if (pos + totalChunk > aInput.Length()) {
      break;
    }
    Span<const uint8_t> chunkData = aInput.subspan(pos + 8, dataLen);

    if (memcmp(chunkType.data(), kChunkEXIf, 4) == 0 ||
        IsMetadataTextChunk(chunkType, chunkData)) {
      return true;
    }

    if (memcmp(chunkType.data(), "IEND", 4) == 0) {
      break;
    }
    pos += totalChunk;
  }
  return false;
}

static Maybe<nsTArray<uint8_t>> StripExifFromPng(Span<const uint8_t> aInput) {
  if (!PngHasExif(aInput)) {
    return Nothing();
  }

  glean::exif_strip::exif_detected.Get("png"_ns).Add(1);

  nsTArray<uint8_t> out;
  out.AppendElements(kPngSignature, 8);

  size_t pos = 8;
  while (pos + 12 <= aInput.Length()) {
    uint32_t dataLen =
        (uint32_t(aInput[pos]) << 24) | (uint32_t(aInput[pos + 1]) << 16) |
        (uint32_t(aInput[pos + 2]) << 8) | uint32_t(aInput[pos + 3]);
    Span<const uint8_t> chunkType = aInput.subspan(pos + 4, 4);
    size_t totalChunk = size_t(12) + size_t(dataLen);
    if (pos + totalChunk > aInput.Length()) {
      break;
    }
    Span<const uint8_t> chunkData = aInput.subspan(pos + 8, dataLen);

    bool isIEND = (memcmp(chunkType.data(), "IEND", 4) == 0);
    bool skip = (memcmp(chunkType.data(), kChunkEXIf, 4) == 0 ||
                 IsMetadataTextChunk(chunkType, chunkData));

    if (!skip) {
      out.AppendElements(aInput.data() + pos, totalChunk);
    }

    pos += totalChunk;
    if (isIEND) {
      break;
    }
  }

  glean::exif_strip::exif_stripped.Get("png"_ns).Add(1);
  return Some(std::move(out));
}

// ---------------------------------------------------------------------------
// GPS-only stripping (preserves all other EXIF tags)
// ---------------------------------------------------------------------------

static constexpr uint16_t kGpsIfdTag = 0x8825;

static uint16_t ReadU16(Span<const uint8_t> aBuf, size_t aOff, bool aLE) {
  if (aLE) {
    return uint16_t(aBuf[aOff]) | (uint16_t(aBuf[aOff + 1]) << 8);
  }
  return (uint16_t(aBuf[aOff]) << 8) | uint16_t(aBuf[aOff + 1]);
}

static uint32_t ReadU32(Span<const uint8_t> aBuf, size_t aOff, bool aLE) {
  if (aLE) {
    return uint32_t(aBuf[aOff]) | (uint32_t(aBuf[aOff + 1]) << 8) |
           (uint32_t(aBuf[aOff + 2]) << 16) | (uint32_t(aBuf[aOff + 3]) << 24);
  }
  return (uint32_t(aBuf[aOff]) << 24) | (uint32_t(aBuf[aOff + 1]) << 16) |
         (uint32_t(aBuf[aOff + 2]) << 8) | uint32_t(aBuf[aOff + 3]);
}

// Parse TIFF header, walk IFD0, check for GPS IFD pointer tag (0x8825).
static bool TiffHasGps(Span<const uint8_t> aTiff) {
  if (aTiff.Length() < 8) {
    return false;
  }
  bool le;
  if (aTiff[0] == 'I' && aTiff[1] == 'I') {
    le = true;
  } else if (aTiff[0] == 'M' && aTiff[1] == 'M') {
    le = false;
  } else {
    return false;
  }
  uint16_t magic = ReadU16(aTiff, 2, le);
  if (magic != 42) {
    return false;
  }
  uint32_t ifd0Off = ReadU32(aTiff, 4, le);
  if (ifd0Off + 2 > aTiff.Length()) {
    return false;
  }
  uint16_t count = ReadU16(aTiff, ifd0Off, le);
  size_t entriesStart = ifd0Off + 2;
  if (entriesStart + size_t(count) * 12 > aTiff.Length()) {
    return false;
  }
  for (uint16_t i = 0; i < count; ++i) {
    uint16_t tag = ReadU16(aTiff, entriesStart + i * 12, le);
    if (tag == kGpsIfdTag) {
      return true;
    }
  }
  return false;
}

// Bytes per element for TIFF types 1-12.
static constexpr uint8_t kTiffTypeSize[] = {
    0,  // 0: unused
    1,  // 1: BYTE
    1,  // 2: ASCII
    2,  // 3: SHORT
    4,  // 4: LONG
    8,  // 5: RATIONAL (2x LONG)
    1,  // 6: SBYTE
    1,  // 7: UNDEFINED
    2,  // 8: SSHORT
    4,  // 9: SLONG
    8,  // 10: SRATIONAL
    4,  // 11: FLOAT
    8,  // 12: DOUBLE
};

// Zero the GPS sub-IFD directory and all data it references.
static void ZeroGpsSubIfd(Span<uint8_t> aBuf, uint32_t aIfdOff, bool aLE) {
  if (aIfdOff + 2 > aBuf.Length()) {
    return;
  }
  uint16_t count = ReadU16(aBuf, aIfdOff, aLE);
  size_t dirStart = aIfdOff;
  size_t dirSize = size_t(2) + size_t(count) * 12 + 4;
  if (dirStart + dirSize > aBuf.Length()) {
    return;
  }

  // Zero external data referenced by each entry.
  for (uint16_t i = 0; i < count; ++i) {
    size_t entryOff = aIfdOff + 2 + i * 12;
    uint16_t type = ReadU16(aBuf, entryOff + 2, aLE);
    uint32_t cnt = ReadU32(aBuf, entryOff + 4, aLE);
    if (type == 0 || type > 12) {
      continue;
    }
    uint32_t totalBytes = uint32_t(kTiffTypeSize[type]) * cnt;
    if (totalBytes > 4) {
      uint32_t dataOff = ReadU32(aBuf, entryOff + 8, aLE);
      if (dataOff < aBuf.Length()) {
        size_t toZero =
            std::min(size_t(totalBytes), aBuf.Length() - size_t(dataOff));
        memset(aBuf.data() + dataOff, 0, toZero);
      }
    }
  }

  // Zero the sub-IFD directory itself.
  memset(aBuf.data() + dirStart, 0, dirSize);
}

// Remove GPS IFD pointer entry from IFD0. Returns modified TIFF or Nothing().
static Maybe<nsTArray<uint8_t>> StripGpsFromTiff(Span<const uint8_t> aTiff) {
  if (aTiff.Length() < 8) {
    return Nothing();
  }
  bool le;
  if (aTiff[0] == 'I' && aTiff[1] == 'I') {
    le = true;
  } else if (aTiff[0] == 'M' && aTiff[1] == 'M') {
    le = false;
  } else {
    return Nothing();
  }
  uint16_t magic = ReadU16(aTiff, 2, le);
  if (magic != 42) {
    return Nothing();
  }
  uint32_t ifd0Off = ReadU32(aTiff, 4, le);
  if (ifd0Off + 2 > aTiff.Length()) {
    return Nothing();
  }
  uint16_t count = ReadU16(aTiff, ifd0Off, le);
  size_t entriesStart = ifd0Off + 2;
  size_t entriesEnd = entriesStart + size_t(count) * 12;
  if (entriesEnd > aTiff.Length()) {
    return Nothing();
  }

  // Find GPS entry index.
  int gpsIdx = -1;
  for (uint16_t i = 0; i < count; ++i) {
    uint16_t tag = ReadU16(aTiff, entriesStart + i * 12, le);
    if (tag == kGpsIfdTag) {
      gpsIdx = i;
      break;
    }
  }
  if (gpsIdx < 0) {
    return Nothing();
  }

  nsTArray<uint8_t> out;
  out.SetCapacity(aTiff.Length());
  out.AppendElements(aTiff.data(), aTiff.Length());

  // Read the GPS sub-IFD offset from the IFD0 entry before zeroing it.
  size_t gpsEntryOff = entriesStart + size_t(gpsIdx) * 12;
  uint32_t gpsIfdOff = ReadU32(Span<const uint8_t>(out), gpsEntryOff + 8, le);

  // Zero the IFD0 GPS pointer entry.
  memset(out.Elements() + gpsEntryOff, 0, 12);

  // Zero the GPS sub-IFD and all external data it references so no GPS
  // values remain recoverable via hex search.
  ZeroGpsSubIfd(Span(out), gpsIfdOff, le);

  return Some(std::move(out));
}

static bool JpegHasGps(Span<const uint8_t> aInput) {
  if (aInput.Length() < 2 || aInput[0] != 0xFF || aInput[1] != kJpegSoi2) {
    return false;
  }
  size_t pos = 2;
  while (pos + 3 < aInput.Length()) {
    if (aInput[pos] != 0xFF) {
      break;
    }
    uint8_t marker = aInput[pos + 1];
    if (marker == kJpegEoi2) {
      break;
    }
    if (marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7)) {
      pos += 2;
      continue;
    }
    if (pos + 3 >= aInput.Length()) {
      break;
    }
    uint16_t segLen =
        (uint16_t(aInput[pos + 2]) << 8) | uint16_t(aInput[pos + 3]);
    if (segLen < 2) {
      break;
    }
    if (marker == kJpegApp1 && segLen >= 2 + sizeof(kExifHeader)) {
      size_t dataStart = pos + 4;
      if (dataStart + sizeof(kExifHeader) <= aInput.Length() &&
          memcmp(aInput.data() + dataStart, kExifHeader, sizeof(kExifHeader)) ==
              0) {
        size_t tiffStart = dataStart + sizeof(kExifHeader);
        size_t tiffLen = segLen - 2 - sizeof(kExifHeader);
        if (tiffStart + tiffLen <= aInput.Length()) {
          return TiffHasGps(aInput.subspan(tiffStart, tiffLen));
        }
      }
    }
    pos += 2 + segLen;
  }
  return false;
}

static Maybe<nsTArray<uint8_t>> StripGpsFromJpeg(Span<const uint8_t> aInput) {
  if (!JpegHasGps(aInput)) {
    return Nothing();
  }

  glean::exif_strip::exif_detected.Get("jpeg"_ns).Add(1);

  nsTArray<uint8_t> out;
  out.AppendElements(aInput.data(), 2);

  size_t pos = 2;
  while (pos < aInput.Length()) {
    if (aInput[pos] != 0xFF) {
      out.AppendElements(aInput.data() + pos, aInput.Length() - pos);
      break;
    }
    if (pos + 1 >= aInput.Length()) {
      break;
    }
    uint8_t marker = aInput[pos + 1];

    if (marker == kJpegEoi2) {
      out.AppendElements(aInput.data() + pos, aInput.Length() - pos);
      break;
    }

    if (marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7)) {
      out.AppendElements(aInput.data() + pos, 2);
      pos += 2;
      continue;
    }

    if (pos + 3 >= aInput.Length()) {
      break;
    }
    uint16_t segLen =
        (uint16_t(aInput[pos + 2]) << 8) | uint16_t(aInput[pos + 3]);
    if (segLen < 2) {
      break;
    }
    size_t totalSegSize = 2 + segLen;
    if (pos + totalSegSize > aInput.Length()) {
      break;
    }

    bool isExifApp1 = (marker == kJpegApp1) &&
                      (segLen >= 2 + (uint16_t)sizeof(kExifHeader)) &&
                      (memcmp(aInput.data() + pos + 4, kExifHeader,
                              sizeof(kExifHeader)) == 0);

    if (isExifApp1) {
      size_t tiffStart = pos + 4 + sizeof(kExifHeader);
      size_t tiffLen = segLen - 2 - sizeof(kExifHeader);
      auto stripped = StripGpsFromTiff(aInput.subspan(tiffStart, tiffLen));
      if (stripped.isSome()) {
        // Rebuild APP1 segment with modified TIFF payload.
        nsTArray<uint8_t>& newTiff = stripped.ref();
        uint16_t newSegLen =
            uint16_t(2 + sizeof(kExifHeader) + newTiff.Length());
        out.AppendElement(0xFF);
        out.AppendElement(kJpegApp1);
        out.AppendElement(uint8_t(newSegLen >> 8));
        out.AppendElement(uint8_t(newSegLen));
        out.AppendElements(kExifHeader, sizeof(kExifHeader));
        out.AppendElements(newTiff.Elements(), newTiff.Length());
      } else {
        out.AppendElements(aInput.data() + pos, totalSegSize);
      }
    } else {
      out.AppendElements(aInput.data() + pos, totalSegSize);
    }
    pos += totalSegSize;
  }

  glean::exif_strip::exif_stripped.Get("jpeg"_ns).Add(1);
  return Some(std::move(out));
}

static bool PngHasGps(Span<const uint8_t> aInput) {
  if (aInput.Length() < 8 || memcmp(aInput.data(), kPngSignature, 8) != 0) {
    return false;
  }
  size_t pos = 8;
  while (pos + 12 <= aInput.Length()) {
    uint32_t dataLen =
        (uint32_t(aInput[pos]) << 24) | (uint32_t(aInput[pos + 1]) << 16) |
        (uint32_t(aInput[pos + 2]) << 8) | uint32_t(aInput[pos + 3]);
    Span<const uint8_t> chunkType = aInput.subspan(pos + 4, 4);
    size_t totalChunk = size_t(12) + size_t(dataLen);
    if (pos + totalChunk > aInput.Length()) {
      break;
    }
    if (memcmp(chunkType.data(), kChunkEXIf, 4) == 0 && dataLen >= 8) {
      Span<const uint8_t> tiff = aInput.subspan(pos + 8, dataLen);
      if (TiffHasGps(tiff)) {
        return true;
      }
    }
    if (memcmp(chunkType.data(), "IEND", 4) == 0) {
      break;
    }
    pos += totalChunk;
  }
  return false;
}

static uint32_t PngCrc(Span<const uint8_t> aChunkTypeAndData) {
  return crc32(crc32(0L, Z_NULL, 0), aChunkTypeAndData.data(),
               aChunkTypeAndData.Length());
}

static Maybe<nsTArray<uint8_t>> StripGpsFromPng(Span<const uint8_t> aInput) {
  if (!PngHasGps(aInput)) {
    return Nothing();
  }

  glean::exif_strip::exif_detected.Get("png"_ns).Add(1);

  nsTArray<uint8_t> out;
  out.AppendElements(kPngSignature, 8);

  size_t pos = 8;
  while (pos + 12 <= aInput.Length()) {
    uint32_t dataLen =
        (uint32_t(aInput[pos]) << 24) | (uint32_t(aInput[pos + 1]) << 16) |
        (uint32_t(aInput[pos + 2]) << 8) | uint32_t(aInput[pos + 3]);
    Span<const uint8_t> chunkType = aInput.subspan(pos + 4, 4);
    size_t totalChunk = size_t(12) + size_t(dataLen);
    if (pos + totalChunk > aInput.Length()) {
      break;
    }

    bool isIEND = (memcmp(chunkType.data(), "IEND", 4) == 0);

    if (memcmp(chunkType.data(), kChunkEXIf, 4) == 0 && dataLen >= 8) {
      Span<const uint8_t> tiff = aInput.subspan(pos + 8, dataLen);
      auto stripped = StripGpsFromTiff(tiff);
      if (stripped.isSome()) {
        nsTArray<uint8_t>& newTiff = stripped.ref();
        uint32_t newLen = newTiff.Length();
        // Length (big-endian)
        out.AppendElement(uint8_t(newLen >> 24));
        out.AppendElement(uint8_t(newLen >> 16));
        out.AppendElement(uint8_t(newLen >> 8));
        out.AppendElement(uint8_t(newLen));
        // Chunk type
        out.AppendElements(kChunkEXIf, 4);
        // Chunk data
        out.AppendElements(newTiff.Elements(), newTiff.Length());
        // CRC over type + data
        size_t crcStart = out.Length() - 4 - newLen;
        uint32_t crc = PngCrc(Span(out).subspan(crcStart, 4 + newLen));
        out.AppendElement(uint8_t(crc >> 24));
        out.AppendElement(uint8_t(crc >> 16));
        out.AppendElement(uint8_t(crc >> 8));
        out.AppendElement(uint8_t(crc));
      } else {
        out.AppendElements(aInput.data() + pos, totalChunk);
      }
    } else {
      out.AppendElements(aInput.data() + pos, totalChunk);
    }

    pos += totalChunk;
    if (isIEND) {
      break;
    }
  }

  glean::exif_strip::exif_stripped.Get("png"_ns).Add(1);
  return Some(std::move(out));
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool HasExif(Span<const uint8_t> aInput, const nsAString& aMimeType) {
  if (aMimeType.EqualsLiteral("image/jpeg")) {
    return JpegHasExif(aInput);
  }
  if (aMimeType.EqualsLiteral("image/png")) {
    return PngHasExif(aInput);
  }
  return false;
}

Maybe<nsTArray<uint8_t>> StripExif(Span<const uint8_t> aInput,
                                   const nsAString& aMimeType) {
  if (aMimeType.EqualsLiteral("image/jpeg")) {
    return StripExifFromJpeg(aInput);
  }
  if (aMimeType.EqualsLiteral("image/png")) {
    return StripExifFromPng(aInput);
  }
  return Nothing();
}

bool HasGps(Span<const uint8_t> aInput, const nsAString& aMimeType) {
  if (aMimeType.EqualsLiteral("image/jpeg")) {
    return JpegHasGps(aInput);
  }
  if (aMimeType.EqualsLiteral("image/png")) {
    return PngHasGps(aInput);
  }
  return false;
}

Maybe<nsTArray<uint8_t>> StripGps(Span<const uint8_t> aInput,
                                  const nsAString& aMimeType) {
  if (aMimeType.EqualsLiteral("image/jpeg")) {
    return StripGpsFromJpeg(aInput);
  }
  if (aMimeType.EqualsLiteral("image/png")) {
    return StripGpsFromPng(aInput);
  }
  return Nothing();
}

static BrowserChild* GetBrowserChildFromGlobal(nsIGlobalObject* aGlobal) {
  auto* win = aGlobal ? aGlobal->GetAsInnerWindow() : nullptr;
  return win ? BrowserChild::GetFrom(win) : nullptr;
}

namespace {

// Runnable dispatched to the stream-transport IO thread. Reads the file stream,
// strips EXIF, then dispatches a completion runnable back to the main thread.
class ExifStripIORunnable final : public Runnable {
 public:
  ExifStripIORunnable(already_AddRefed<nsIInputStream> aStream,
                      const nsAString& aContentType, const nsAString& aFileName,
                      nsMainThreadPtrHandle<File> aOriginalFile,
                      nsMainThreadPtrHandle<nsIGlobalObject> aGlobal,
                      ExifStrippedCallback&& aCallback,
                      nsCOMPtr<nsIEventTarget> aMainThread)
      : Runnable("dom::ExifStripIORunnable"),
        mStream(aStream),
        mContentType(aContentType),
        mFileName(aFileName),
        mOriginalFile(std::move(aOriginalFile)),
        mGlobal(std::move(aGlobal)),
        mCallback(std::move(aCallback)),
        mMainThread(std::move(aMainThread)) {}

  NS_IMETHOD Run() override {
    MOZ_ASSERT(!NS_IsMainThread());

    void* buf = nullptr;
    uint64_t len = 0;

    nsAutoCString bytes;
    if (NS_SUCCEEDED(NS_ReadInputStreamToString(mStream, bytes, -1))) {
      auto input = Span(reinterpret_cast<const uint8_t*>(bytes.BeginReading()),
                        bytes.Length());
      Maybe<nsTArray<uint8_t>> stripped = StripGps(input, mContentType);
      if (stripped.isSome()) {
        nsTArray<uint8_t>& data = stripped.ref();
        len = data.Length();
        buf = moz_xmalloc(len);
        memcpy(buf, data.Elements(), len);
      }
    }

    DispatchComplete(buf, len);
    return NS_OK;
  }

 private:
  void DispatchComplete(void* aBuf, uint64_t aLen) {
    // Move all main-thread-only members into the completion runnable so
    // nothing is released on the IO thread when this runnable destructs.
    nsCOMPtr<nsIRunnable> runnable = NS_NewRunnableFunction(
        "dom::ExifStripIORunnable::Complete",
        [originalFile = std::move(mOriginalFile), global = std::move(mGlobal),
         callback = std::move(mCallback), contentType = nsString(mContentType),
         fileName = nsString(mFileName), buf = aBuf, len = aLen]() mutable {
          MOZ_ASSERT(NS_IsMainThread());
          if (!buf) {
            callback(RefPtr<File>(originalFile.get()).forget());
            return;
          }
          RefPtr<File> newFile = File::CreateMemoryFileWithLastModifiedNow(
              global.get(), buf, len, fileName, contentType);
          if (!newFile) {
            free(buf);
            callback(RefPtr<File>(originalFile.get()).forget());
            return;
          }
          callback(newFile.forget());
        });
    mMainThread->Dispatch(runnable.forget(), NS_DISPATCH_NORMAL);
  }

  nsCOMPtr<nsIInputStream> mStream;
  nsString mContentType;
  nsString mFileName;
  nsMainThreadPtrHandle<File> mOriginalFile;
  nsMainThreadPtrHandle<nsIGlobalObject> mGlobal;
  ExifStrippedCallback mCallback;
  nsCOMPtr<nsIEventTarget> mMainThread;
};

}  // namespace

already_AddRefed<File> MaybeStripExifFromFile(File* aFile,
                                              nsIGlobalObject* aGlobal) {
  MOZ_ASSERT(NS_IsMainThread());

  nsAutoString contentType;
  aFile->GetType(contentType);

  if (!contentType.EqualsLiteral("image/jpeg") &&
      !contentType.EqualsLiteral("image/png")) {
    return do_AddRef(aFile);
  }

  auto mode =
      static_cast<ExifStripMode>(StaticPrefs::privacy_removeExifOnUpload());

  ErrorResult rv;
  nsCOMPtr<nsIInputStream> stream;
  aFile->Impl()->CreateInputStream(getter_AddRefs(stream), rv);
  if (rv.Failed() || !stream) {
    rv.SuppressException();
    return do_AddRef(aFile);
  }

  nsAutoCString bytes;
  if (NS_FAILED(NS_ReadInputStreamToString(stream, bytes, -1))) {
    return do_AddRef(aFile);
  }

  auto input = Span(reinterpret_cast<const uint8_t*>(bytes.BeginReading()),
                    bytes.Length());

  if (mode == ExifStripMode::Never) {
    if (HasGps(input, contentType)) {
      BrowserChild* bc = GetBrowserChildFromGlobal(aGlobal);
      if (bc) {
        bc->SendNotifyExifDetected(static_cast<int32_t>(mode));
      }
    }
    return do_AddRef(aFile);
  }

  // Ask or Always: strip GPS data.
  Maybe<nsTArray<uint8_t>> stripped = StripGps(input, contentType);
  if (stripped.isNothing()) {
    return do_AddRef(aFile);
  }

  nsAutoString fileName;
  aFile->GetName(fileName);

  nsTArray<uint8_t>& data = stripped.ref();
  void* buf = moz_xmalloc(data.Length());
  memcpy(buf, data.Elements(), data.Length());

  RefPtr<File> newFile = File::CreateMemoryFileWithLastModifiedNow(
      aGlobal, buf, data.Length(), fileName, contentType);
  if (!newFile) {
    free(buf);
    return do_AddRef(aFile);
  }

  BrowserChild* bc = GetBrowserChildFromGlobal(aGlobal);
  if (bc) {
    bc->SendNotifyExifDetected(static_cast<int32_t>(mode));
  }

  return newFile.forget();
}

void MaybeStripExifFromFileAsync(File* aFile, nsIGlobalObject* aGlobal,
                                 ExifStrippedCallback&& aCallback) {
  MOZ_ASSERT(NS_IsMainThread());

  nsAutoString contentType;
  aFile->GetType(contentType);

  auto mode =
      static_cast<ExifStripMode>(StaticPrefs::privacy_removeExifOnUpload());

  if (mode == ExifStripMode::Never) {
    if (contentType.EqualsLiteral("image/jpeg") ||
        contentType.EqualsLiteral("image/png")) {
      ErrorResult rv;
      nsCOMPtr<nsIInputStream> stream;
      aFile->Impl()->CreateInputStream(getter_AddRefs(stream), rv);
      if (!rv.Failed() && stream) {
        nsAutoCString bytes;
        if (NS_SUCCEEDED(NS_ReadInputStreamToString(stream, bytes, -1))) {
          auto input =
              Span(reinterpret_cast<const uint8_t*>(bytes.BeginReading()),
                   bytes.Length());
          if (HasGps(input, contentType)) {
            BrowserChild* bc = GetBrowserChildFromGlobal(aGlobal);
            if (bc) {
              bc->SendNotifyExifDetected(static_cast<int32_t>(mode));
            }
          }
        }
      } else {
        rv.SuppressException();
      }
    }
    aCallback(do_AddRef(aFile));
    return;
  }

  if (contentType.EqualsLiteral("image/jpeg")) {
    glean::exif_strip::image_input_type.Get("jpeg"_ns).Add(1);
  } else if (contentType.EqualsLiteral("image/png")) {
    glean::exif_strip::image_input_type.Get("png"_ns).Add(1);
  } else {
    glean::exif_strip::image_input_type.Get("other"_ns).Add(1);
    aCallback(do_AddRef(aFile));
    return;
  }

  ErrorResult rv;
  nsCOMPtr<nsIInputStream> stream;
  aFile->Impl()->CreateInputStream(getter_AddRefs(stream), rv);
  if (rv.Failed() || !stream) {
    rv.SuppressException();
    aCallback(do_AddRef(aFile));
    return;
  }

  nsAutoString fileName;
  aFile->GetName(fileName);

  nsCOMPtr<nsIEventTarget> ioThread =
      do_GetService(NS_STREAMTRANSPORTSERVICE_CONTRACTID);
  if (!ioThread) {
    aCallback(do_AddRef(aFile));
    return;
  }

  BrowserChild* bc = GetBrowserChildFromGlobal(aGlobal);
  RefPtr<BrowserChild> bcRef = bc;
  int32_t modeInt = static_cast<int32_t>(mode);
  ExifStrippedCallback wrappedCallback =
      [origCallback = std::move(aCallback), origFile = RefPtr<File>(aFile),
       bcRef = std::move(bcRef),
       modeInt](already_AddRefed<File> aResultFile) mutable {
        RefPtr<File> resultFile = aResultFile;
        if (resultFile != origFile && bcRef) {
          bcRef->SendNotifyExifDetected(modeInt);
        }
        origCallback(resultFile.forget());
      };

  nsMainThreadPtrHandle<File> fileHandle(
      new nsMainThreadPtrHolder<File>("ExifStripFile", aFile));
  nsMainThreadPtrHandle<nsIGlobalObject> globalHandle(
      new nsMainThreadPtrHolder<nsIGlobalObject>("ExifStripGlobal", aGlobal));

  auto runnable = MakeRefPtr<ExifStripIORunnable>(
      stream.forget(), contentType, fileName, std::move(fileHandle),
      std::move(globalHandle), std::move(wrappedCallback),
      GetMainThreadSerialEventTarget());

  ioThread->Dispatch(runnable.forget(), NS_DISPATCH_NORMAL);
}

// ---------------------------------------------------------------------------
// Produce both original + stripped versions (for Ask mode)
// ---------------------------------------------------------------------------

namespace {

class ExifStripBothIORunnable final : public Runnable {
 public:
  ExifStripBothIORunnable(already_AddRefed<nsIInputStream> aStream,
                          const nsAString& aContentType,
                          const nsAString& aFileName,
                          nsMainThreadPtrHandle<File> aOriginalFile,
                          nsMainThreadPtrHandle<nsIGlobalObject> aGlobal,
                          ExifStripBothCallback&& aCallback,
                          nsCOMPtr<nsIEventTarget> aMainThread)
      : Runnable("dom::ExifStripBothIORunnable"),
        mStream(aStream),
        mContentType(aContentType),
        mFileName(aFileName),
        mOriginalFile(std::move(aOriginalFile)),
        mGlobal(std::move(aGlobal)),
        mCallback(std::move(aCallback)),
        mMainThread(std::move(aMainThread)) {}

  NS_IMETHOD Run() override {
    MOZ_ASSERT(!NS_IsMainThread());

    bool hasGps = false;
    void* buf = nullptr;
    uint64_t len = 0;

    nsAutoCString bytes;
    if (NS_SUCCEEDED(NS_ReadInputStreamToString(mStream, bytes, -1))) {
      auto input = Span(reinterpret_cast<const uint8_t*>(bytes.BeginReading()),
                        bytes.Length());
      hasGps = HasGps(input, mContentType);
      if (hasGps) {
        Maybe<nsTArray<uint8_t>> stripped = StripGps(input, mContentType);
        if (stripped.isSome()) {
          nsTArray<uint8_t>& data = stripped.ref();
          len = data.Length();
          buf = moz_xmalloc(len);
          memcpy(buf, data.Elements(), len);
        }
      }
    }

    DispatchComplete(hasGps, buf, len);
    return NS_OK;
  }

 private:
  void DispatchComplete(bool aHasGps, void* aBuf, uint64_t aLen) {
    nsCOMPtr<nsIRunnable> runnable = NS_NewRunnableFunction(
        "dom::ExifStripBothIORunnable::Complete",
        [originalFile = std::move(mOriginalFile), global = std::move(mGlobal),
         callback = std::move(mCallback), contentType = nsString(mContentType),
         fileName = nsString(mFileName), hasGps = aHasGps, buf = aBuf,
         len = aLen]() mutable {
          MOZ_ASSERT(NS_IsMainThread());
          ExifStripResult result;
          result.original = originalFile.get();
          result.hasGps = hasGps;
          if (!hasGps || !buf) {
            if (buf) {
              free(buf);
            }
            result.stripped = originalFile.get();
            callback(std::move(result));
            return;
          }
          RefPtr<File> newFile = File::CreateMemoryFileWithLastModifiedNow(
              global.get(), buf, len, fileName, contentType);
          if (!newFile) {
            free(buf);
            result.stripped = originalFile.get();
          } else {
            result.stripped = newFile;
          }
          callback(std::move(result));
        });
    mMainThread->Dispatch(runnable.forget(), NS_DISPATCH_NORMAL);
  }

  nsCOMPtr<nsIInputStream> mStream;
  nsString mContentType;
  nsString mFileName;
  nsMainThreadPtrHandle<File> mOriginalFile;
  nsMainThreadPtrHandle<nsIGlobalObject> mGlobal;
  ExifStripBothCallback mCallback;
  nsCOMPtr<nsIEventTarget> mMainThread;
};

}  // namespace

void StripGpsProduceBoth(File* aFile, nsIGlobalObject* aGlobal,
                         ExifStripBothCallback&& aCallback) {
  MOZ_ASSERT(NS_IsMainThread());

  nsAutoString contentType;
  aFile->GetType(contentType);

  if (!contentType.EqualsLiteral("image/jpeg") &&
      !contentType.EqualsLiteral("image/png")) {
    ExifStripResult result;
    result.original = aFile;
    result.stripped = aFile;
    result.hasGps = false;
    aCallback(std::move(result));
    return;
  }

  ErrorResult rv;
  nsCOMPtr<nsIInputStream> stream;
  aFile->Impl()->CreateInputStream(getter_AddRefs(stream), rv);
  if (rv.Failed() || !stream) {
    rv.SuppressException();
    ExifStripResult result;
    result.original = aFile;
    result.stripped = aFile;
    result.hasGps = false;
    aCallback(std::move(result));
    return;
  }

  nsAutoString fileName;
  aFile->GetName(fileName);

  nsCOMPtr<nsIEventTarget> ioThread =
      do_GetService(NS_STREAMTRANSPORTSERVICE_CONTRACTID);
  if (!ioThread) {
    ExifStripResult result;
    result.original = aFile;
    result.stripped = aFile;
    result.hasGps = false;
    aCallback(std::move(result));
    return;
  }

  nsMainThreadPtrHandle<File> fileHandle(
      new nsMainThreadPtrHolder<File>("ExifStripBothFile", aFile));
  nsMainThreadPtrHandle<nsIGlobalObject> globalHandle(
      new nsMainThreadPtrHolder<nsIGlobalObject>("ExifStripBothGlobal",
                                                 aGlobal));

  auto runnable = MakeRefPtr<ExifStripBothIORunnable>(
      stream.forget(), contentType, fileName, std::move(fileHandle),
      std::move(globalHandle), std::move(aCallback),
      GetMainThreadSerialEventTarget());

  ioThread->Dispatch(runnable.forget(), NS_DISPATCH_NORMAL);
}

// ---------------------------------------------------------------------------
// Re-strip on pref change
// ---------------------------------------------------------------------------

static RefPtr<HTMLInputElement> sLastExifInput;
static nsCOMPtr<nsIObserver> sExifPrefObserver;

namespace {

class ExifPrefObserver final : public nsIObserver {
 public:
  NS_DECL_ISUPPORTS
  NS_DECL_NSIOBSERVER
 private:
  ~ExifPrefObserver() = default;
};

NS_IMPL_ISUPPORTS(ExifPrefObserver, nsIObserver)

NS_IMETHODIMP
ExifPrefObserver::Observe(nsISupports*, const char* aTopic, const char16_t*) {
  if (strcmp(aTopic, "nsPref:changed") != 0) {
    return NS_OK;
  }
  int32_t pref = StaticPrefs::privacy_removeExifOnUpload();
  if (pref == 0) {
    return NS_OK;
  }
  RefPtr<HTMLInputElement> input = sLastExifInput;
  sLastExifInput = nullptr;
  Preferences::RemoveObserver(this, "privacy.removeExifOnUpload"_ns);
  sExifPrefObserver = nullptr;
  // Only re-strip for "always" (2). "Ask" (1) would re-trigger the dialog.
  if (input && pref == 2) {
    ReStripExifOnInput(input);
  }
  return NS_OK;
}

}  // namespace

void RegisterExifReStripObserver(HTMLInputElement* aInput) {
  MOZ_ASSERT(NS_IsMainThread());
  if (sExifPrefObserver) {
    Preferences::RemoveObserver(sExifPrefObserver,
                                "privacy.removeExifOnUpload"_ns);
    sExifPrefObserver = nullptr;
  }
  sLastExifInput = aInput;
  sExifPrefObserver = new ExifPrefObserver();
  Preferences::AddStrongObserver(sExifPrefObserver,
                                 "privacy.removeExifOnUpload"_ns);
}

namespace {

struct ReStripBatch {
  NS_INLINE_DECL_REFCOUNTING(ReStripBatch)
  nsTArray<OwningFileOrDirectory> files;
  uint32_t pending = 0;
  RefPtr<HTMLInputElement> input;

 private:
  ~ReStripBatch() = default;
};

}  // namespace

void ReStripExifOnInput(HTMLInputElement* aInput) {
  MOZ_ASSERT(NS_IsMainThread());
  if (!aInput) {
    return;
  }

  nsIGlobalObject* global = aInput->GetOwnerGlobal();
  if (!global) {
    return;
  }

  const nsTArray<OwningFileOrDirectory>& files =
      aInput->GetFilesOrDirectoriesInternal();

  uint32_t pending = 0;
  for (const auto& entry : files) {
    if (entry.IsFile()) {
      nsAutoString ct;
      entry.GetAsFile()->GetType(ct);
      if (ct.EqualsLiteral("image/jpeg") || ct.EqualsLiteral("image/png")) {
        ++pending;
      }
    }
  }

  if (pending == 0) {
    return;
  }

  auto batch = MakeRefPtr<ReStripBatch>();
  batch->files = files.Clone();
  batch->pending = pending;
  batch->input = aInput;

  for (uint32_t i = 0; i < batch->files.Length(); ++i) {
    if (!batch->files[i].IsFile()) {
      continue;
    }
    RefPtr<File> file = batch->files[i].GetAsFile();
    nsAutoString ct;
    file->GetType(ct);
    if (!ct.EqualsLiteral("image/jpeg") && !ct.EqualsLiteral("image/png")) {
      continue;
    }
    MaybeStripExifFromFileAsync(
        file, global, [batch, i](already_AddRefed<File> aResult) mutable {
          MOZ_ASSERT(NS_IsMainThread());
          RefPtr<File> stripped = aResult;
          batch->files[i].SetAsFile() = stripped;
          if (--batch->pending == 0) {
            batch->input->SetFilesOrDirectories(batch->files, false);
          }
        });
  }
}

}  // namespace mozilla::dom
