/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

export let ExifPromptHelper = {
  observe(aSubject, aTopic, aData) {
    let browser;
    if (aSubject instanceof Ci.nsIDOMWindow) {
      browser = aSubject.docShell.chromeEventHandler;
    } else {
      browser = aSubject;
    }

    let window = browser?.ownerGlobal;
    if (!window) {
      return;
    }

    let trustPanel = window.gTrustPanelHandler;
    if (trustPanel) {
      trustPanel.showExifAlert(aData);
    }
  },
};
