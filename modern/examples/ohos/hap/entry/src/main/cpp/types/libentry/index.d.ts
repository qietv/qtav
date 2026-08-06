declare const qtavOHOS: {
  start(h264Media: Uint8Array, hevcMedia: Uint8Array): boolean;
  setForeground(foreground: boolean): void;
  stop(): void;
  status(): string;
};

export default qtavOHOS;
