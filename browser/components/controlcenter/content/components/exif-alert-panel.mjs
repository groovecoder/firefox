/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

import { html, nothing } from "chrome://global/content/vendor/lit.all.mjs";
import { MozLitElement } from "chrome://global/content/lit-utils.mjs";

/**
 * Trust panel component for the EXIF "ask" confirmation dialog.
 * Stripped/detected modes are handled by the main trust panel graphic section.
 */
export default class ExifAlertPanel extends MozLitElement {
  static properties = {
    exifStatus: { type: String },
  };

  constructor() {
    super();
    this.exifStatus = "idle";
  }

  _handleRemove() {
    this.dispatchEvent(
      new CustomEvent("exif-action", {
        bubbles: true,
        detail: { action: "remove" },
      })
    );
  }

  _handleShare() {
    this.dispatchEvent(
      new CustomEvent("exif-action", {
        bubbles: true,
        detail: { action: "share" },
      })
    );
  }

  render() {
    if (this.exifStatus !== "ask") {
      return nothing;
    }

    return html`
      <link
        rel="stylesheet"
        href="chrome://browser/content/controlcenter/components/exif-alert-panel.css"
      />
      <div class="container">
        <div class="card ask">
          <div class="main">
            <img
              src="chrome://browser/skin/trustpanel-graphic-warning.svg"
              alt=""
            />
            <div class="content">
              <h2 class="header" data-l10n-id="trustpanel-exif-ask-header"></h2>
              <p data-l10n-id="trustpanel-exif-ask-description"></p>
            </div>
          </div>
          <moz-button-group>
            <moz-button
              @click=${this._handleShare}
              data-l10n-id="trustpanel-exif-share"
            ></moz-button>
            <moz-button
              type="primary"
              @click=${this._handleRemove}
              data-l10n-id="trustpanel-exif-remove"
            ></moz-button>
          </moz-button-group>
        </div>
      </div>
    `;
  }
}

customElements.define("exif-alert-panel", ExifAlertPanel);
