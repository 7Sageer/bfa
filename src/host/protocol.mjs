export const BASE255_MAX = 255 ** 4 - 1;
export const MAX_FRAME_LENGTH = 0xffffff;

export function encodeNumber(value) {
  if (!Number.isInteger(value) || value < 0 || value > BASE255_MAX) {
    throw new Error('number must be a nonnegative integer fitting in four base-255 digits');
  }
  const encoded = Buffer.allocUnsafe(4);
  for (let i = 3; i >= 0; i--) {
    encoded[i] = value % 255 + 1;
    value = Math.floor(value / 255);
  }
  return encoded;
}

export function decodeNumber(bytes) {
  if (bytes.length !== 4) {
    throw new Error('base-255 field must contain exactly four bytes');
  }
  let value = 0;
  for (const byte of bytes) {
    if (!Number.isInteger(byte) || byte < 1 || byte > 255) {
      throw new Error('invalid byte in base-255 field');
    }
    value = value * 255 + byte - 1;
  }
  return value;
}

function checkFrameLength(length) {
  if (!Number.isInteger(length) || length < 0 || length > MAX_FRAME_LENGTH) {
    throw new Error(`frame payload exceeds the 24-bit BF length limit: ${length}`);
  }
}

export function encodeFrame(payload) {
  checkFrameLength(payload.length);
  const frame = Buffer.allocUnsafe(payload.length + 4);
  encodeNumber(payload.length).copy(frame, 0);
  payload.copy(frame, 4);
  return frame;
}

export class FrameReader {
  constructor(onText = () => {}) {
    this.onText = onText;
    this.state = 'magic';
    this.op = 0;
    this.lengthBytes = [];
    this.payload = null;
    this.offset = 0;
  }

  emitByte(byte) {
    if (this.state === 'magic') {
      if (byte === 255) {
        this.state = 'op';
      } else {
        this.onText(byte);
      }
      return null;
    }
    if (this.state === 'op') {
      this.op = byte;
      this.lengthBytes = [];
      this.state = 'length';
      return null;
    }
    if (this.state === 'length') {
      this.lengthBytes.push(byte);
      if (this.lengthBytes.length < 4) {
        return null;
      }
      const length = decodeNumber(this.lengthBytes);
      checkFrameLength(length);
      this.payload = Buffer.allocUnsafe(length);
      this.offset = 0;
      this.state = 'payload';
      if (length > 0) {
        return null;
      }
    } else {
      this.payload[this.offset++] = byte;
      if (this.offset < this.payload.length) {
        return null;
      }
    }

    const call = { op: this.op, payload: this.payload };
    this.payload = null;
    this.state = 'magic';
    return call;
  }

  finish() {
    if (this.state !== 'magic') {
      throw new Error('Brainfuck program ended with an incomplete host frame');
    }
  }
}
