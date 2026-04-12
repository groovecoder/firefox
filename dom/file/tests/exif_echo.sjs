/* -*- Mode: JavaScript; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

// Echo the raw POST body back as application/octet-stream so the test can
// inspect bytes that Firefox actually sent to the server.

const BinaryInputStream = Components.Constructor(
  "@mozilla.org/binaryinputstream;1",
  "nsIBinaryInputStream",
  "setInputStream"
);
const BinaryOutputStream = Components.Constructor(
  "@mozilla.org/binaryoutputstream;1",
  "nsIBinaryOutputStream",
  "setOutputStream"
);

function handleRequest(request, response) {
  const body = new BinaryInputStream(request.bodyInputStream);
  const bytes = [];
  let avail;
  while ((avail = body.available()) > 0) {
    Array.prototype.push.apply(bytes, body.readByteArray(avail));
  }

  response.setHeader("Content-Type", "application/octet-stream", false);
  const out = new BinaryOutputStream(response.bodyOutputStream);
  out.writeByteArray(bytes, bytes.length);
}
