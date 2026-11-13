'use strict';
// A real 96x64 baseline JPEG (produced by the 3DS encoder) and a helper that grows it to any size by inserting
// COM (comment) segments after SOI, which every decoder skips: handy for testing frames of 60 KB, 100 KB...
const BASE64 =
  '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';

const jpeg = () => Buffer.from(BASE64, 'base64');

// Returns a valid JPEG of at least `size` bytes (the picture is unchanged).
function jpegOfSize(size) {
  const base = jpeg();
  if (base.length >= size) return base;
  const pad = [];
  let have = base.length;
  while (have < size) {
    const body = Math.min(65533, size - have - 4);
    const seg = Buffer.alloc(body + 4);
    seg[0] = 0xff; seg[1] = 0xfe; seg.writeUInt16BE(body + 2, 2);
    pad.push(seg); have += seg.length;
  }
  return Buffer.concat([base.subarray(0, 2), ...pad, base.subarray(2)]);
}

module.exports = { jpeg, jpegOfSize, BASE64 };
