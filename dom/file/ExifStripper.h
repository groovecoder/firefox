/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_ExifStripper_h
#define mozilla_dom_ExifStripper_h

#include <functional>

#include "mozilla/Maybe.h"
#include "mozilla/Span.h"
#include "nsTArray.h"

class nsIGlobalObject;

namespace mozilla::dom {

enum class ExifStripMode : int32_t { Never = 0, Ask = 1, Always = 2 };

class File;
class HTMLInputElement;

// Returns true if the image contains EXIF metadata.
bool HasExif(Span<const uint8_t> aInput, const nsAString& aMimeType);

// Returns a new buffer with EXIF stripped, or Nothing() if the format is
// unsupported or the input is not a valid image of that type.
Maybe<nsTArray<uint8_t>> StripExif(Span<const uint8_t> aInput,
                                   const nsAString& aMimeType);

// Returns true if the image contains GPS location data in its EXIF.
bool HasGps(Span<const uint8_t> aInput, const nsAString& aMimeType);

// Returns a new buffer with GPS IFD removed from EXIF (preserving all other
// EXIF tags), or Nothing() if no GPS data found or format unsupported.
Maybe<nsTArray<uint8_t>> StripGps(Span<const uint8_t> aInput,
                                  const nsAString& aMimeType);

// Synchronously strip EXIF from aFile on the calling thread (main thread).
// Returns a new File if stripping occurred, or the original File unchanged.
already_AddRefed<File> MaybeStripExifFromFile(File* aFile,
                                              nsIGlobalObject* aGlobal);

// Callback type for MaybeStripExifFromFileAsync. Always invoked on the main
// thread with either a stripped File or the original File unchanged.
using ExifStrippedCallback = std::function<void(already_AddRefed<File>)>;

struct ExifStripResult {
  RefPtr<File> original;
  RefPtr<File> stripped;
  bool hasGps;
};

using ExifStripBothCallback = std::function<void(ExifStripResult)>;

// Asynchronously strip EXIF from aFile on the stream-transport IO thread.
// aCallback is always called on the main thread.
void MaybeStripExifFromFileAsync(File* aFile, nsIGlobalObject* aGlobal,
                                 ExifStrippedCallback&& aCallback);

// Asynchronously produce both original and GPS-stripped versions of aFile.
// aCallback receives an ExifStripResult on the main thread.
void StripGpsProduceBoth(File* aFile, nsIGlobalObject* aGlobal,
                         ExifStripBothCallback&& aCallback);

// Store the input element that triggered EXIF detection when the pref is OFF,
// and register a pref observer that will re-strip the files if the user enables
// the pref from the trust panel.
void RegisterExifReStripObserver(HTMLInputElement* aInput);

// Re-strip EXIF from files on the given input element. Called by the pref
// observer when privacy.removeExifOnUpload flips to true.
void ReStripExifOnInput(HTMLInputElement* aInput);

}  // namespace mozilla::dom

#endif  // mozilla_dom_ExifStripper_h
