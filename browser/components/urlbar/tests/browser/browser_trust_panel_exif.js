/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ */

/**
 * Test trust panel EXIF detection and stripping UI.
 */

"use strict";

const FILE_PICKER_PAGE =
  "https://example.com/browser/browser/components/urlbar/tests/browser/dummy_page.html";

add_setup(async function setup() {
  await SpecialPowers.pushPrefEnv({
    set: [
      ["browser.urlbar.scotchBonnet.enableOverride", true],
      ["browser.urlbar.trustPanel.featureGate", true],
      ["browser.tabs.hoverPreview.enabled", false],
      ["dom.filesystem.pathcheck.disabled", true],
    ],
  });
  registerCleanupFunction(async () => {
    await PlacesUtils.history.clear();
  });
});

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

function buildExifJpegBytes() {
  const tiff = [
    0x49,
    0x49,
    0x2a,
    0x00,
    0x08,
    0x00,
    0x00,
    0x00, // TIFF header
    0x02,
    0x00, // IFD0: 2 entries
    0x12,
    0x01,
    0x03,
    0x00,
    0x01,
    0x00,
    0x00,
    0x00,
    0x01,
    0x00,
    0x00,
    0x00,
    0x25,
    0x88,
    0x04,
    0x00,
    0x01,
    0x00,
    0x00,
    0x00,
    0x26,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00, // next IFD = 0
    0x01,
    0x00, // GPS IFD: 1 entry
    0x00,
    0x00,
    0x01,
    0x00,
    0x04,
    0x00,
    0x00,
    0x00,
    0x02,
    0x03,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00, // next IFD = 0
  ];
  const exifHeader = [0x45, 0x78, 0x69, 0x66, 0x00, 0x00]; // "Exif\0\0"
  const segLen = 2 + exifHeader.length + tiff.length;
  return new Uint8Array([
    0xff,
    0xd8, // SOI
    0xff,
    0xe1, // APP1
    segLen >> 8,
    segLen & 0xff,
    ...exifHeader,
    ...tiff,
    0xff,
    0xd9, // EOI
  ]);
}

function buildJpegWithoutExifBytes() {
  return new Uint8Array([
    0xff,
    0xd8, // SOI
    0xff,
    0xe0, // APP0 marker
    0x00,
    0x07, // segment length
    0x4a,
    0x46,
    0x49,
    0x46,
    0x00, // "JFIF\0"
    0xff,
    0xd9, // EOI
  ]);
}

async function writeExifJpegToTempFile() {
  let bytes = buildExifJpegBytes();
  let tmpPath = PathUtils.join(
    PathUtils.tempDir,
    "exif_test_" + Math.random().toString(36).slice(2) + ".jpg"
  );
  await IOUtils.write(tmpPath, bytes);
  registerCleanupFunction(() =>
    IOUtils.remove(tmpPath, { ignoreAbsent: true })
  );
  return tmpPath;
}

async function writeNoExifJpegToTempFile() {
  let bytes = buildJpegWithoutExifBytes();
  let tmpPath = PathUtils.join(
    PathUtils.tempDir,
    "noexif_test_" + Math.random().toString(36).slice(2) + ".jpg"
  );
  await IOUtils.write(tmpPath, bytes);
  registerCleanupFunction(() =>
    IOUtils.remove(tmpPath, { ignoreAbsent: true })
  );
  return tmpPath;
}

// Triggers file picker and waits for the change event (for Always/Never modes).
async function triggerFilePickerInContent(browser, filePath) {
  await SpecialPowers.spawn(browser, [filePath], async function (path) {
    let nsIFile = content.SpecialPowers.Cc[
      "@mozilla.org/file/local;1"
    ].createInstance(content.SpecialPowers.Ci.nsIFile);
    nsIFile.initWithPath(path);

    let MockFilePicker = content.SpecialPowers.MockFilePicker;
    MockFilePicker.init(content.browsingContext);
    MockFilePicker.setFiles([nsIFile]);
    MockFilePicker.returnValue = MockFilePicker.returnOK;

    let input = content.document.createElement("input");
    input.type = "file";
    input.accept = "image/jpeg";
    input.id = "exif-test-input";
    content.document.body.appendChild(input);

    let changed = new Promise(r =>
      input.addEventListener("change", r, { once: true })
    );

    content.SpecialPowers.wrap(content.document).notifyUserGestureActivation();
    input.click();
    await changed;
    MockFilePicker.cleanup();
  });
}

// Triggers file picker without waiting for change (for blocking Ask mode).
async function triggerFilePickerNoWait(browser, filePath) {
  await SpecialPowers.spawn(browser, [filePath], async function (path) {
    let nsIFile = content.SpecialPowers.Cc[
      "@mozilla.org/file/local;1"
    ].createInstance(content.SpecialPowers.Ci.nsIFile);
    nsIFile.initWithPath(path);

    let MockFilePicker = content.SpecialPowers.MockFilePicker;
    MockFilePicker.init(content.browsingContext);
    MockFilePicker.setFiles([nsIFile]);
    MockFilePicker.returnValue = MockFilePicker.returnOK;

    let input = content.document.createElement("input");
    input.type = "file";
    input.accept = "image/jpeg";
    input.id = "exif-test-input";
    content.document.body.appendChild(input);

    content.SpecialPowers.wrap(content.document).notifyUserGestureActivation();
    input.click();
    MockFilePicker.cleanup();
  });
}

// Returns the file count on the input element.
async function getInputFileCount(browser) {
  return SpecialPowers.spawn(browser, [], async function () {
    let input = content.document.getElementById("exif-test-input");
    return input ? input.files.length : 0;
  });
}

// Returns the byte length of the first file on the input element.
async function getInputFileSize(browser) {
  return SpecialPowers.spawn(browser, [], async function () {
    let input = content.document.getElementById("exif-test-input");
    if (!input || !input.files.length) {
      return 0;
    }
    let buf = await input.files[0].arrayBuffer();
    return buf.byteLength;
  });
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

add_task(async function test_exif_detected_never_mode() {
  await SpecialPowers.pushPrefEnv({
    set: [["privacy.removeExifOnUpload", 0]],
  });

  let tab = await BrowserTestUtils.openNewForegroundTab(
    gBrowser,
    FILE_PICKER_PAGE,
    true
  );
  let filePath = await writeExifJpegToTempFile();

  let observerPromise = TestUtils.topicObserved("exif-detected");
  let popupShown = BrowserTestUtils.waitForEvent(document, "popupshown");

  await triggerFilePickerInContent(tab.linkedBrowser, filePath);

  let [, data] = await observerPromise;
  Assert.equal(data, "detected", "Observer data is 'detected' in Never mode");

  await popupShown;

  let hint = document.getElementById("confirmation-hint");
  Assert.equal(
    hint.getAttribute("data-message-id"),
    "confirmation-hint-exif-shared-with-location",
    "Neutral toast shown for Never mode"
  );

  let trustPanel = document.getElementById("trustpanel-popup");
  Assert.ok(
    !trustPanel || trustPanel.state !== "open",
    "Trust panel popup did not open for Never mode"
  );

  BrowserTestUtils.removeTab(tab);
  await SpecialPowers.popPrefEnv();
});

add_task(async function test_exif_ask_mode_blocking() {
  await SpecialPowers.pushPrefEnv({
    set: [["privacy.removeExifOnUpload", 1]],
  });

  let tab = await BrowserTestUtils.openNewForegroundTab(
    gBrowser,
    FILE_PICKER_PAGE,
    true
  );
  let filePath = await writeExifJpegToTempFile();

  let observerPromise = TestUtils.topicObserved("exif-detected");
  let popupShown = BrowserTestUtils.waitForEvent(document, "popupshown");

  await triggerFilePickerNoWait(tab.linkedBrowser, filePath);

  let [, data] = await observerPromise;
  Assert.equal(data, "ask", "Observer data is 'ask' in Ask mode");

  await popupShown;

  let exifPanel = document.getElementById("trustpanel-exif-alert-section");
  Assert.ok(!exifPanel.hidden, "EXIF alert component is visible in Ask mode");
  Assert.equal(exifPanel.exifStatus, "ask", "Component status is 'ask'");

  // File should NOT be delivered to the page yet.
  let fileCount = await getInputFileCount(tab.linkedBrowser);
  Assert.equal(fileCount, 0, "No file delivered before user choice");

  await BrowserTestUtils.waitForCondition(
    () => exifPanel.shadowRoot?.querySelector(".header"),
    "Wait for shadow DOM render"
  );

  let shareBtn = exifPanel.shadowRoot.querySelector("moz-button:not([type])");
  Assert.ok(shareBtn, "Share button exists");

  let removeBtn = exifPanel.shadowRoot.querySelector(
    "moz-button[type=primary]"
  );
  Assert.ok(removeBtn, "Remove button exists");

  await UrlbarTestUtils.closeTrustPanel(window);
  BrowserTestUtils.removeTab(tab);
  await SpecialPowers.popPrefEnv();
});

add_task(async function test_ask_remove_delivers_stripped_file() {
  await SpecialPowers.pushPrefEnv({
    set: [["privacy.removeExifOnUpload", 1]],
  });

  let tab = await BrowserTestUtils.openNewForegroundTab(
    gBrowser,
    FILE_PICKER_PAGE,
    true
  );
  let filePath = await writeExifJpegToTempFile();
  let originalSize = (await IOUtils.read(filePath)).byteLength;

  let observerPromise = TestUtils.topicObserved("exif-detected");
  let popupShown = BrowserTestUtils.waitForEvent(document, "popupshown");

  await triggerFilePickerNoWait(tab.linkedBrowser, filePath);
  await observerPromise;
  await popupShown;

  let exifPanel = document.getElementById("trustpanel-exif-alert-section");
  await BrowserTestUtils.waitForCondition(
    () => exifPanel.shadowRoot?.querySelector("moz-button[type=primary]"),
    "Wait for shadow DOM render"
  );

  // Click Remove.
  let popupHidden = BrowserTestUtils.waitForEvent(document, "popuphidden");
  let removeBtn = exifPanel.shadowRoot.querySelector(
    "moz-button[type=primary]"
  );
  EventUtils.synthesizeMouseAtCenter(removeBtn, {});
  await popupHidden;

  // Wait for the change event to fire in content.
  await BrowserTestUtils.waitForCondition(
    async () => (await getInputFileCount(tab.linkedBrowser)) > 0,
    "File delivered after Remove click"
  );

  let deliveredSize = await getInputFileSize(tab.linkedBrowser);
  Assert.less(
    deliveredSize,
    originalSize,
    "Delivered file is smaller (GPS stripped)"
  );

  BrowserTestUtils.removeTab(tab);
  await SpecialPowers.popPrefEnv();
});

add_task(async function test_ask_share_delivers_original_file() {
  await SpecialPowers.pushPrefEnv({
    set: [["privacy.removeExifOnUpload", 1]],
  });

  let tab = await BrowserTestUtils.openNewForegroundTab(
    gBrowser,
    FILE_PICKER_PAGE,
    true
  );
  let filePath = await writeExifJpegToTempFile();
  let originalSize = (await IOUtils.read(filePath)).byteLength;

  let observerPromise = TestUtils.topicObserved("exif-detected");
  let popupShown = BrowserTestUtils.waitForEvent(document, "popupshown");

  await triggerFilePickerNoWait(tab.linkedBrowser, filePath);
  await observerPromise;
  await popupShown;

  let exifPanel = document.getElementById("trustpanel-exif-alert-section");
  await BrowserTestUtils.waitForCondition(
    () => exifPanel.shadowRoot?.querySelector("moz-button:not([type])"),
    "Wait for shadow DOM render"
  );

  // Click Share (include location).
  let popupHidden = BrowserTestUtils.waitForEvent(document, "popuphidden");
  let shareBtn = exifPanel.shadowRoot.querySelector("moz-button:not([type])");
  EventUtils.synthesizeMouseAtCenter(shareBtn, {});
  await popupHidden;

  // Wait for the change event to fire in content.
  await BrowserTestUtils.waitForCondition(
    async () => (await getInputFileCount(tab.linkedBrowser)) > 0,
    "File delivered after Share click"
  );

  let deliveredSize = await getInputFileSize(tab.linkedBrowser);
  Assert.equal(
    deliveredSize,
    originalSize,
    "Delivered file matches original size (not stripped)"
  );

  BrowserTestUtils.removeTab(tab);
  await SpecialPowers.popPrefEnv();
});

add_task(async function test_exif_stripped_always_mode() {
  await SpecialPowers.pushPrefEnv({
    set: [["privacy.removeExifOnUpload", 2]],
  });

  let tab = await BrowserTestUtils.openNewForegroundTab(
    gBrowser,
    FILE_PICKER_PAGE,
    true
  );
  let filePath = await writeExifJpegToTempFile();

  let observerPromise = TestUtils.topicObserved("exif-detected");

  await triggerFilePickerInContent(tab.linkedBrowser, filePath);

  let [, data] = await observerPromise;
  Assert.equal(data, "stripped", "Observer data is 'stripped' in Always mode");

  let popup = document.getElementById("trustpanel-popup");
  Assert.ok(
    !popup || popup.state !== "open",
    "Trust panel popup did not open for Always mode"
  );

  BrowserTestUtils.removeTab(tab);
  await SpecialPowers.popPrefEnv();
});

add_task(async function test_exif_stripped_panel_header_override() {
  await SpecialPowers.pushPrefEnv({
    set: [["privacy.removeExifOnUpload", 2]],
  });

  let tab = await BrowserTestUtils.openNewForegroundTab(
    gBrowser,
    FILE_PICKER_PAGE,
    true
  );
  let filePath = await writeExifJpegToTempFile();

  let observerPromise = TestUtils.topicObserved("exif-detected");
  await triggerFilePickerInContent(tab.linkedBrowser, filePath);
  await observerPromise;

  await UrlbarTestUtils.openTrustPanel(window);

  let header = document.getElementById("trustpanel-header");
  Assert.equal(
    header.getAttribute("data-l10n-id"),
    "trustpanel-exif-stripped-header",
    "Panel header shows stripped message"
  );

  let description = document.getElementById("trustpanel-description");
  Assert.equal(
    description.getAttribute("data-l10n-id"),
    "trustpanel-exif-stripped-description",
    "Panel description shows stripped message"
  );

  let exifPanel = document.getElementById("trustpanel-exif-alert-section");
  Assert.ok(exifPanel.hidden, "EXIF alert component is hidden when stripped");

  await UrlbarTestUtils.closeTrustPanel(window);
  BrowserTestUtils.removeTab(tab);
  await SpecialPowers.popPrefEnv();
});

add_task(async function test_no_exif_no_notification() {
  await SpecialPowers.pushPrefEnv({
    set: [["privacy.removeExifOnUpload", 2]],
  });

  let tab = await BrowserTestUtils.openNewForegroundTab(
    gBrowser,
    FILE_PICKER_PAGE,
    true
  );
  let filePath = await writeNoExifJpegToTempFile();

  let notified = false;
  let observer = {
    observe() {
      notified = true;
    },
  };
  Services.obs.addObserver(observer, "exif-detected");

  await triggerFilePickerInContent(tab.linkedBrowser, filePath);

  // eslint-disable-next-line mozilla/no-arbitrary-setTimeout
  await new Promise(r => setTimeout(r, 1000));

  Assert.ok(!notified, "No exif-detected notification for image without EXIF");

  Services.obs.removeObserver(observer, "exif-detected");
  BrowserTestUtils.removeTab(tab);
  await SpecialPowers.popPrefEnv();
});

add_task(async function test_navigation_clears_exif_state() {
  await SpecialPowers.pushPrefEnv({
    set: [["privacy.removeExifOnUpload", 2]],
  });

  let tab = await BrowserTestUtils.openNewForegroundTab(
    gBrowser,
    FILE_PICKER_PAGE,
    true
  );
  let filePath = await writeExifJpegToTempFile();

  let observerPromise = TestUtils.topicObserved("exif-detected");
  await triggerFilePickerInContent(tab.linkedBrowser, filePath);
  await observerPromise;

  BrowserTestUtils.startLoadingURIString(
    tab.linkedBrowser,
    "https://example.org"
  );
  await BrowserTestUtils.browserLoaded(tab.linkedBrowser);

  await UrlbarTestUtils.openTrustPanel(window);

  let header = document.getElementById("trustpanel-header");
  Assert.notEqual(
    header.getAttribute("data-l10n-id"),
    "trustpanel-exif-stripped-header",
    "Header is not EXIF override after navigation"
  );

  await UrlbarTestUtils.closeTrustPanel(window);
  BrowserTestUtils.removeTab(tab);
  await SpecialPowers.popPrefEnv();
});
