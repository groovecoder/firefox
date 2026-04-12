/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "gtest/gtest.h"
#include "mozilla/Span.h"
#include "mozilla/dom/ExifStripper.h"
#include "nsLiteralString.h"
#include "nsTArray.h"

using namespace mozilla;
using namespace mozilla::dom;

// ---------------------------------------------------------------------------
// Helpers to build minimal JPEG / PNG test data
// ---------------------------------------------------------------------------

static nsTArray<uint8_t> MakeJpegWithExif() {
  nsTArray<uint8_t> data;
  // SOI
  data.AppendElement(0xFF);
  data.AppendElement(0xD8);
  // APP1/EXIF segment: marker(2) + len(2) + "Exif\0\0"(6) + dummy(4)
  // len = 2 + 6 + 4 = 12
  uint8_t app1[] = {0xFF, 0xE1, 0x00, 0x0C, 'E',  'x',  'i',
                    'f',  0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  data.AppendElements(app1, sizeof(app1));
  // EOI
  data.AppendElement(0xFF);
  data.AppendElement(0xD9);
  return data;
}

static nsTArray<uint8_t> MakeJpegWithoutExif() {
  nsTArray<uint8_t> data;
  // SOI
  data.AppendElement(0xFF);
  data.AppendElement(0xD8);
  // A harmless APP0/JFIF segment
  // len = 2 + 5 = 7
  uint8_t app0[] = {0xFF, 0xE0, 0x00, 0x07, 'J', 'F', 'I', 'F', 0x00};
  data.AppendElements(app0, sizeof(app0));
  // EOI
  data.AppendElement(0xFF);
  data.AppendElement(0xD9);
  return data;
}

static nsTArray<uint8_t> MakePngWithExif() {
  nsTArray<uint8_t> data;
  // PNG signature
  const uint8_t sig[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
  data.AppendElements(sig, 8);

  // Minimal IHDR chunk (13 bytes of data)
  // len(4) + type(4) + data(13) + crc(4)
  const uint8_t ihdr[] = {
      0x00, 0x00, 0x00, 0x0D,                          // length = 13
      'I',  'H',  'D',  'R',  0x00, 0x00, 0x00, 0x01,  // width = 1
      0x00, 0x00, 0x00, 0x01,                          // height = 1
      0x08, 0x02, 0x00, 0x00, 0x00,                    // 8-bit RGB
      0x90, 0x77, 0x53, 0xDE  // CRC (not verified in our code)
  };
  data.AppendElements(ihdr, sizeof(ihdr));

  // eXIf chunk with 4 bytes of dummy data
  const uint8_t exif[] = {
      0x00, 0x00, 0x00, 0x04,                          // length = 4
      'e',  'X',  'I',  'f',  0x49, 0x49, 0x2A, 0x00,  // dummy TIFF header
      0x00, 0x00, 0x00, 0x00                           // CRC placeholder
  };
  data.AppendElements(exif, sizeof(exif));

  // IEND chunk
  const uint8_t iend[] = {
      0x00, 0x00, 0x00, 0x00,                         // length = 0
      'I',  'E',  'N',  'D',  0xAE, 0x42, 0x60, 0x82  // CRC
  };
  data.AppendElements(iend, sizeof(iend));
  return data;
}

static nsTArray<uint8_t> MakePngWithoutExif() {
  nsTArray<uint8_t> data;
  const uint8_t sig[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
  data.AppendElements(sig, 8);

  const uint8_t ihdr[] = {0x00, 0x00, 0x00, 0x0D, 'I',  'H',  'D',  'R',  0x00,
                          0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x02,
                          0x00, 0x00, 0x00, 0x90, 0x77, 0x53, 0xDE};
  data.AppendElements(ihdr, sizeof(ihdr));

  const uint8_t iend[] = {0x00, 0x00, 0x00, 0x00, 'I',  'E',
                          'N',  'D',  0xAE, 0x42, 0x60, 0x82};
  data.AppendElements(iend, sizeof(iend));
  return data;
}

// ---------------------------------------------------------------------------
// JPEG tests
// ---------------------------------------------------------------------------

TEST(ExifStripper, JpegWithExifStripped)
{
  auto input = MakeJpegWithExif();
  auto result = StripExif(Span(input), u"image/jpeg"_ns);
  ASSERT_TRUE(result.isSome());

  // Output must start with SOI
  ASSERT_GE(result.ref().Length(), 2u);
  EXPECT_EQ(result.ref()[0], 0xFF);
  EXPECT_EQ(result.ref()[1], 0xD8);

  // The APP1/EXIF marker must not appear in the output
  const nsTArray<uint8_t>& out = result.ref();
  const uint8_t exifMarkerSeq[] = {'E', 'x', 'i', 'f', 0x00, 0x00};
  bool found = false;
  for (size_t i = 0; i + 6 <= out.Length(); ++i) {
    if (memcmp(out.Elements() + i, exifMarkerSeq, 6) == 0) {
      found = true;
      break;
    }
  }
  EXPECT_FALSE(found) << "EXIF header should have been removed";
}

TEST(ExifStripper, JpegWithoutExifUnchanged)
{
  auto input = MakeJpegWithoutExif();
  auto result = StripExif(Span(input), u"image/jpeg"_ns);
  EXPECT_TRUE(result.isNothing()) << "No stripping should occur without EXIF";
}

TEST(ExifStripper, JpegTruncatedInput)
{
  nsTArray<uint8_t> truncated;
  truncated.AppendElement(0xFF);
  truncated.AppendElement(0xD8);
  truncated.AppendElement(0xFF);
  // Truncated — no crash expected
  auto result = StripExif(Span(truncated), u"image/jpeg"_ns);
  EXPECT_TRUE(result.isNothing());
}

// ---------------------------------------------------------------------------
// PNG tests
// ---------------------------------------------------------------------------

TEST(ExifStripper, PngWithExifStripped)
{
  auto input = MakePngWithExif();
  auto result = StripExif(Span(input), u"image/png"_ns);
  ASSERT_TRUE(result.isSome());

  // Output must start with PNG signature
  const uint8_t sig[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
  ASSERT_GE(result.ref().Length(), 8u);
  EXPECT_EQ(memcmp(result.ref().Elements(), sig, 8), 0);

  // eXIf chunk must not be present
  const nsTArray<uint8_t>& out = result.ref();
  const uint8_t exifChunkType[] = {'e', 'X', 'I', 'f'};
  bool found = false;
  for (size_t i = 0; i + 4 <= out.Length(); ++i) {
    if (memcmp(out.Elements() + i, exifChunkType, 4) == 0) {
      found = true;
      break;
    }
  }
  EXPECT_FALSE(found) << "eXIf chunk should have been removed";
}

TEST(ExifStripper, PngWithoutExifUnchanged)
{
  auto input = MakePngWithoutExif();
  auto result = StripExif(Span(input), u"image/png"_ns);
  EXPECT_TRUE(result.isNothing()) << "No stripping should occur without EXIF";
}

TEST(ExifStripper, PngTruncatedInput)
{
  nsTArray<uint8_t> truncated;
  const uint8_t sig[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
  truncated.AppendElements(sig, 8);
  // Truncated after signature — no crash expected
  auto result = StripExif(Span(truncated), u"image/png"_ns);
  EXPECT_TRUE(result.isNothing());
}

TEST(ExifStripper, PngChunkLenOverflow)
{
  // PNG with a valid eXIf chunk followed by a chunk whose 4-byte length field
  // is 0xFFFFFFF8. Without the size_t cast, "12 + dataLen" wraps to 4 in
  // uint32_t arithmetic and the subsequent bounds check passes incorrectly,
  // causing Span::subspan to MOZ_RELEASE_ASSERT. No crash expected.
  nsTArray<uint8_t> data;
  const uint8_t sig[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
  data.AppendElements(sig, 8);

  // Minimal IHDR (25 bytes)
  const uint8_t ihdr[] = {0x00, 0x00, 0x00, 0x0D, 'I',  'H',  'D',  'R',  0x00,
                          0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x02,
                          0x00, 0x00, 0x00, 0x90, 0x77, 0x53, 0xDE};
  data.AppendElements(ihdr, sizeof(ihdr));

  // Zero-length eXIf chunk (sets foundExif=true in pre-scan)
  const uint8_t exif[] = {0x00, 0x00, 0x00, 0x00, 'e',  'X',
                          'I',  'f',  0x00, 0x00, 0x00, 0x00};
  data.AppendElements(exif, sizeof(exif));

  // tEXt chunk with dataLen=0xFFFFFFF8 — triggers the overflow
  const uint8_t evil[] = {0xFF, 0xFF, 0xFF, 0xF8, 't',  'E',
                          'X',  't',  0x00, 0x00, 0x00, 0x00};
  data.AppendElements(evil, sizeof(evil));

  // IEND
  const uint8_t iend[] = {0x00, 0x00, 0x00, 0x00, 'I',  'E',
                          'N',  'D',  0xAE, 0x42, 0x60, 0x82};
  data.AppendElements(iend, sizeof(iend));

  // Must not crash
  auto result = StripExif(Span(data), u"image/png"_ns);
  // The evil chunk's totalChunk overflows past the buffer, so the parser
  // breaks out of the loop — the eXIf chunk is still found so stripping
  // is attempted, but the evil chunk is never emitted.
  (void)result;
}

// ---------------------------------------------------------------------------
// Unsupported MIME type
// ---------------------------------------------------------------------------

TEST(ExifStripper, UnsupportedMimeType)
{
  nsTArray<uint8_t> data = {0x01, 0x02, 0x03, 0x04};
  auto result = StripExif(Span(data), u"image/webp"_ns);
  EXPECT_TRUE(result.isNothing());
}

TEST(ExifStripper, EmptyInput)
{
  nsTArray<uint8_t> empty;
  EXPECT_TRUE(StripExif(Span(empty), u"image/jpeg"_ns).isNothing());
  EXPECT_TRUE(StripExif(Span(empty), u"image/png"_ns).isNothing());
}

// ---------------------------------------------------------------------------
// HasExif tests
// ---------------------------------------------------------------------------

TEST(ExifStripper, JpegWithExifDetected)
{
  auto input = MakeJpegWithExif();
  EXPECT_TRUE(HasExif(Span(input), u"image/jpeg"_ns));
}

TEST(ExifStripper, JpegWithoutExifNotDetected)
{
  auto input = MakeJpegWithoutExif();
  EXPECT_FALSE(HasExif(Span(input), u"image/jpeg"_ns));
}

TEST(ExifStripper, PngWithExifDetected)
{
  auto input = MakePngWithExif();
  EXPECT_TRUE(HasExif(Span(input), u"image/png"_ns));
}

TEST(ExifStripper, PngWithoutExifNotDetected)
{
  auto input = MakePngWithoutExif();
  EXPECT_FALSE(HasExif(Span(input), u"image/png"_ns));
}

TEST(ExifStripper, HasExifUnsupportedMimeType)
{
  nsTArray<uint8_t> data = {0x01, 0x02, 0x03, 0x04};
  EXPECT_FALSE(HasExif(Span(data), u"image/webp"_ns));
}

TEST(ExifStripper, HasExifEmptyInput)
{
  nsTArray<uint8_t> empty;
  EXPECT_FALSE(HasExif(Span(empty), u"image/jpeg"_ns));
  EXPECT_FALSE(HasExif(Span(empty), u"image/png"_ns));
}

// ---------------------------------------------------------------------------
// GPS stripping helpers
// ---------------------------------------------------------------------------

// Build a little-endian TIFF with IFD0 containing Orientation (0x0112) and
// GPS IFD pointer (0x8825). The GPS sub-IFD has one dummy entry.
static nsTArray<uint8_t> MakeTiffWithGps(bool aLittleEndian) {
  nsTArray<uint8_t> t;

  // TIFF header: byte order + magic 42 + IFD0 offset (= 8)
  if (aLittleEndian) {
    uint8_t hdr[] = {'I', 'I', 0x2A, 0x00, 0x08, 0x00, 0x00, 0x00};
    t.AppendElements(hdr, 8);
  } else {
    uint8_t hdr[] = {'M', 'M', 0x00, 0x2A, 0x00, 0x00, 0x00, 0x08};
    t.AppendElements(hdr, 8);
  }

  // IFD0 at offset 8: 2 entries
  // Entry 0: Orientation (tag=0x0112, type=SHORT=3, count=1, value=1)
  // Entry 1: GPSInfo (tag=0x8825, type=LONG=4, count=1, value=offset to GPS
  // IFD) Next IFD offset: 0 (no IFD1)
  uint16_t count = 2;
  // GPS IFD will be at offset 8 + 2 + 2*12 + 4 = 38
  uint32_t gpsIfdOff = 38;

  if (aLittleEndian) {
    // count
    t.AppendElement(uint8_t(count));
    t.AppendElement(uint8_t(count >> 8));
    // Entry 0: Orientation
    uint8_t e0[] = {0x12, 0x01, 0x03, 0x00, 0x01, 0x00,
                    0x00, 0x00, 0x01, 0x00, 0x00, 0x00};
    t.AppendElements(e0, 12);
    // Entry 1: GPSInfo
    uint8_t e1[] = {0x25,
                    0x88,
                    0x04,
                    0x00,
                    0x01,
                    0x00,
                    0x00,
                    0x00,
                    uint8_t(gpsIfdOff),
                    uint8_t(gpsIfdOff >> 8),
                    uint8_t(gpsIfdOff >> 16),
                    uint8_t(gpsIfdOff >> 24)};
    t.AppendElements(e1, 12);
    // Next IFD offset: 0
    uint8_t z[] = {0x00, 0x00, 0x00, 0x00};
    t.AppendElements(z, 4);
  } else {
    // count
    t.AppendElement(uint8_t(count >> 8));
    t.AppendElement(uint8_t(count));
    // Entry 0: Orientation
    uint8_t e0[] = {0x01, 0x12, 0x00, 0x03, 0x00, 0x00,
                    0x00, 0x01, 0x00, 0x01, 0x00, 0x00};
    t.AppendElements(e0, 12);
    // Entry 1: GPSInfo
    uint8_t e1[] = {0x88,
                    0x25,
                    0x00,
                    0x04,
                    0x00,
                    0x00,
                    0x00,
                    0x01,
                    uint8_t(gpsIfdOff >> 24),
                    uint8_t(gpsIfdOff >> 16),
                    uint8_t(gpsIfdOff >> 8),
                    uint8_t(gpsIfdOff)};
    t.AppendElements(e1, 12);
    // Next IFD offset: 0
    uint8_t z[] = {0x00, 0x00, 0x00, 0x00};
    t.AppendElements(z, 4);
  }

  // GPS sub-IFD at offset 38: 1 entry (GPSVersionID tag=0x0000)
  if (aLittleEndian) {
    uint8_t gps[] = {0x01, 0x00,  // count=1
                     0x00, 0x00, 0x01, 0x00, 0x04, 0x00, 0x00, 0x00, 0x02,
                     0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};  // next IFD=0
    t.AppendElements(gps, sizeof(gps));
  } else {
    uint8_t gps[] = {0x00, 0x01,  // count=1
                     0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x04, 0x02,
                     0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};  // next IFD=0
    t.AppendElements(gps, sizeof(gps));
  }

  return t;
}

// Build a TIFF with only Orientation — no GPS.
static nsTArray<uint8_t> MakeTiffNoGps() {
  nsTArray<uint8_t> t;
  uint8_t hdr[] = {'I', 'I', 0x2A, 0x00, 0x08, 0x00, 0x00, 0x00};
  t.AppendElements(hdr, 8);
  // IFD0: 1 entry (Orientation)
  uint8_t ifd[] = {0x01, 0x00,  // count=1
                   0x12, 0x01, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
                   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};  // next IFD=0
  t.AppendElements(ifd, sizeof(ifd));
  return t;
}

static nsTArray<uint8_t> WrapTiffInJpeg(Span<const uint8_t> aTiff) {
  nsTArray<uint8_t> data;
  data.AppendElement(0xFF);
  data.AppendElement(0xD8);
  // APP1 segment
  uint16_t segLen = uint16_t(2 + 6 + aTiff.Length());
  uint8_t segHdr[] = {
      0xFF, 0xE1, uint8_t(segLen >> 8), uint8_t(segLen), 'E', 'x', 'i', 'f',
      0x00, 0x00};
  data.AppendElements(segHdr, sizeof(segHdr));
  data.AppendElements(aTiff.data(), aTiff.Length());
  data.AppendElement(0xFF);
  data.AppendElement(0xD9);
  return data;
}

static nsTArray<uint8_t> WrapTiffInPng(Span<const uint8_t> aTiff) {
  nsTArray<uint8_t> data;
  const uint8_t sig[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
  data.AppendElements(sig, 8);

  const uint8_t ihdr[] = {0x00, 0x00, 0x00, 0x0D, 'I',  'H',  'D',  'R',  0x00,
                          0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x02,
                          0x00, 0x00, 0x00, 0x90, 0x77, 0x53, 0xDE};
  data.AppendElements(ihdr, sizeof(ihdr));

  // eXIf chunk
  uint32_t len = aTiff.Length();
  data.AppendElement(uint8_t(len >> 24));
  data.AppendElement(uint8_t(len >> 16));
  data.AppendElement(uint8_t(len >> 8));
  data.AppendElement(uint8_t(len));
  data.AppendElements("eXIf", 4);
  data.AppendElements(aTiff.data(), aTiff.Length());
  // CRC placeholder (not validated by our stripper)
  data.AppendElement(0x00);
  data.AppendElement(0x00);
  data.AppendElement(0x00);
  data.AppendElement(0x00);

  const uint8_t iend[] = {0x00, 0x00, 0x00, 0x00, 'I',  'E',
                          'N',  'D',  0xAE, 0x42, 0x60, 0x82};
  data.AppendElements(iend, sizeof(iend));
  return data;
}

// ---------------------------------------------------------------------------
// GPS detection tests
// ---------------------------------------------------------------------------

TEST(ExifStripper, JpegHasGpsDetected)
{
  auto tiff = MakeTiffWithGps(true);
  auto input = WrapTiffInJpeg(Span(tiff));
  EXPECT_TRUE(HasGps(Span(input), u"image/jpeg"_ns));
}

TEST(ExifStripper, JpegNoGpsNotDetected)
{
  auto tiff = MakeTiffNoGps();
  auto input = WrapTiffInJpeg(Span(tiff));
  EXPECT_FALSE(HasGps(Span(input), u"image/jpeg"_ns));
}

TEST(ExifStripper, JpegWithoutExifNoGps)
{
  auto input = MakeJpegWithoutExif();
  EXPECT_FALSE(HasGps(Span(input), u"image/jpeg"_ns));
}

TEST(ExifStripper, PngHasGpsDetected)
{
  auto tiff = MakeTiffWithGps(true);
  auto input = WrapTiffInPng(Span(tiff));
  EXPECT_TRUE(HasGps(Span(input), u"image/png"_ns));
}

TEST(ExifStripper, PngNoGpsNotDetected)
{
  auto tiff = MakeTiffNoGps();
  auto input = WrapTiffInPng(Span(tiff));
  EXPECT_FALSE(HasGps(Span(input), u"image/png"_ns));
}

TEST(ExifStripper, HasGpsEmptyInput)
{
  nsTArray<uint8_t> empty;
  EXPECT_FALSE(HasGps(Span(empty), u"image/jpeg"_ns));
  EXPECT_FALSE(HasGps(Span(empty), u"image/png"_ns));
}

TEST(ExifStripper, HasGpsUnsupportedMimeType)
{
  nsTArray<uint8_t> data = {0x01, 0x02};
  EXPECT_FALSE(HasGps(Span(data), u"image/webp"_ns));
}

// ---------------------------------------------------------------------------
// GPS stripping tests
// ---------------------------------------------------------------------------

TEST(ExifStripper, JpegGpsStripped)
{
  auto tiff = MakeTiffWithGps(true);
  auto input = WrapTiffInJpeg(Span(tiff));
  auto result = StripGps(Span(input), u"image/jpeg"_ns);
  ASSERT_TRUE(result.isSome());

  // Output starts with SOI
  ASSERT_GE(result.ref().Length(), 2u);
  EXPECT_EQ(result.ref()[0], 0xFF);
  EXPECT_EQ(result.ref()[1], 0xD8);

  // GPS should be gone
  EXPECT_FALSE(HasGps(Span(result.ref()), u"image/jpeg"_ns));

  // EXIF header should still be present
  EXPECT_TRUE(HasExif(Span(result.ref()), u"image/jpeg"_ns));
}

TEST(ExifStripper, JpegNoGpsUnchanged)
{
  auto tiff = MakeTiffNoGps();
  auto input = WrapTiffInJpeg(Span(tiff));
  auto result = StripGps(Span(input), u"image/jpeg"_ns);
  EXPECT_TRUE(result.isNothing()) << "No GPS means nothing to strip";
}

TEST(ExifStripper, JpegGpsBigEndian)
{
  auto tiff = MakeTiffWithGps(false);
  auto input = WrapTiffInJpeg(Span(tiff));
  EXPECT_TRUE(HasGps(Span(input), u"image/jpeg"_ns));

  auto result = StripGps(Span(input), u"image/jpeg"_ns);
  ASSERT_TRUE(result.isSome());
  EXPECT_FALSE(HasGps(Span(result.ref()), u"image/jpeg"_ns));
  EXPECT_TRUE(HasExif(Span(result.ref()), u"image/jpeg"_ns));
}

TEST(ExifStripper, PngGpsStripped)
{
  auto tiff = MakeTiffWithGps(true);
  auto input = WrapTiffInPng(Span(tiff));
  auto result = StripGps(Span(input), u"image/png"_ns);
  ASSERT_TRUE(result.isSome());

  const uint8_t sig[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
  ASSERT_GE(result.ref().Length(), 8u);
  EXPECT_EQ(memcmp(result.ref().Elements(), sig, 8), 0);

  EXPECT_FALSE(HasGps(Span(result.ref()), u"image/png"_ns));
}

TEST(ExifStripper, PngNoGpsUnchanged)
{
  auto tiff = MakeTiffNoGps();
  auto input = WrapTiffInPng(Span(tiff));
  auto result = StripGps(Span(input), u"image/png"_ns);
  EXPECT_TRUE(result.isNothing()) << "No GPS means nothing to strip";
}

TEST(ExifStripper, GpsStripPreservesOrientation)
{
  auto tiff = MakeTiffWithGps(true);
  auto input = WrapTiffInJpeg(Span(tiff));
  auto result = StripGps(Span(input), u"image/jpeg"_ns);
  ASSERT_TRUE(result.isSome());

  // The EXIF APP1 segment should still be present (Orientation tag survives).
  const nsTArray<uint8_t>& out = result.ref();
  const uint8_t exifHeader[] = {'E', 'x', 'i', 'f', 0x00, 0x00};
  bool foundExif = false;
  for (size_t i = 0; i + 6 <= out.Length(); ++i) {
    if (memcmp(out.Elements() + i, exifHeader, 6) == 0) {
      foundExif = true;
      break;
    }
  }
  EXPECT_TRUE(foundExif) << "EXIF header must survive GPS stripping";

  // Orientation tag (0x0112) should be in the output TIFF.
  // In little-endian, this appears as bytes 0x12 0x01.
  bool foundOrientation = false;
  for (size_t i = 0; i + 2 <= out.Length(); ++i) {
    if (out[i] == 0x12 && out[i + 1] == 0x01) {
      foundOrientation = true;
      break;
    }
  }
  EXPECT_TRUE(foundOrientation) << "Orientation tag must survive GPS stripping";
}

TEST(ExifStripper, GpsStripSameSizeOutput)
{
  auto tiff = MakeTiffWithGps(true);
  auto input = WrapTiffInJpeg(Span(tiff));
  auto result = StripGps(Span(input), u"image/jpeg"_ns);
  ASSERT_TRUE(result.isSome());
  EXPECT_EQ(result.ref().Length(), input.Length())
      << "In-place zeroing must not change file size";
}

TEST(ExifStripper, GpsSubIfdDataZeroed)
{
  // The GPS sub-IFD in our test TIFF starts at offset 38 (LE).
  // After stripping, those bytes (and the IFD0 GPS entry) must be zero.
  auto tiff = MakeTiffWithGps(true);
  auto input = WrapTiffInJpeg(Span(tiff));
  auto result = StripGps(Span(input), u"image/jpeg"_ns);
  ASSERT_TRUE(result.isSome());

  // Find the TIFF payload inside the JPEG APP1 segment.
  // SOI(2) + marker(2) + len(2) + "Exif\0\0"(6) = offset 12.
  const nsTArray<uint8_t>& out = result.ref();
  ASSERT_GT(out.Length(), size_t(12 + 56));
  size_t tiffBase = 12;

  // GPS sub-IFD was at TIFF offset 38 (18 bytes: count(2) + 1 entry(12) +
  // next(4)).
  bool gpsSubIfdAllZero = true;
  for (size_t i = 0; i < 18; ++i) {
    if (out[tiffBase + 38 + i] != 0) {
      gpsSubIfdAllZero = false;
      break;
    }
  }
  EXPECT_TRUE(gpsSubIfdAllZero)
      << "GPS sub-IFD must be zeroed, not just orphaned";

  // The IFD0 GPS entry (second entry at offset 8+2+12 = 22, 12 bytes) must
  // also be zeroed.
  bool gpsEntryAllZero = true;
  for (size_t i = 0; i < 12; ++i) {
    if (out[tiffBase + 22 + i] != 0) {
      gpsEntryAllZero = false;
      break;
    }
  }
  EXPECT_TRUE(gpsEntryAllZero) << "IFD0 GPS pointer entry must be zeroed";
}

TEST(ExifStripper, GpsSubIfdDataZeroedBigEndian)
{
  auto tiff = MakeTiffWithGps(false);
  auto input = WrapTiffInJpeg(Span(tiff));
  auto result = StripGps(Span(input), u"image/jpeg"_ns);
  ASSERT_TRUE(result.isSome());

  const nsTArray<uint8_t>& out = result.ref();
  size_t tiffBase = 12;

  bool gpsSubIfdAllZero = true;
  for (size_t i = 0; i < 18; ++i) {
    if (out[tiffBase + 38 + i] != 0) {
      gpsSubIfdAllZero = false;
      break;
    }
  }
  EXPECT_TRUE(gpsSubIfdAllZero) << "GPS sub-IFD must be zeroed (big-endian)";
}
