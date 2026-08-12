export interface PlayerTrack {
  index: number;
  label: string;
  codec: string;
  language: string;
  title: string;
  bitRate: number;
  channels: number;
  sampleRate: number;
  external: boolean;
}

export interface PlayerSnapshot {
  surfaceReady: boolean;
  playing: boolean;
  seekable: boolean;
  source: string;
  state: string;
  mediaStatus: string;
  subtitle: string;
  videoCodec: string;
  audioCodec: string;
  renderAPI: string;
  renderPreference: string;
  decodePreference: string;
  hdrPreference: string;
  decoderAPI: string;
  inputColorSpace: string;
  outputColorSpace: string;
  outputFormat: string;
  error: string;
  hdrInput: boolean;
  dolbyVisionInput: boolean;
  hdrOutput: boolean;
  toneMappedToSdr: boolean;
  positionMs: number;
  durationMs: number;
  playbackRate: number;
  bufferingProgress: number;
  fps: number;
  width: number;
  height: number;
  activeAudioTrack: number;
  activeSubtitleTrack: number;
  decodedFrames: number;
  hardwareFrames: number;
  softwareFrames: number;
  renderedFrames: number;
  droppedFrames: number;
  opaqueExternalImports: number;
  externalFormatWorkaroundImports: number;
  externalNormalizationPasses: number;
  nativeBuffersAcquired: number;
  frameAvailableCallbacks: number;
  outputsReleasedAfterGpu: number;
  lastVulkanSourceFormat: number;
  lastExternalFormat: number;
  audioTracks: PlayerTrack[];
  subtitleTracks: PlayerTrack[];
}

export interface PlayerProgressSnapshot {
  playing: boolean;
  mediaStatus: string;
  subtitle: string;
  error: string;
  positionMs: number;
  bufferingProgress: number;
}

export interface QtAVPlayerNativeContext {
  openUrl(url: string): boolean;
  openLocalFd(fd: number, displayName: string): boolean;
  setPlaying(playing: boolean): void;
  stop(): void;
  seek(positionMs: number): boolean;
  setRate(rate: number): boolean;
  setDecodeMode(mode: string): boolean;
  setRenderMode(mode: string): boolean;
  setHdrMode(mode: string): boolean;
  selectAudioTrack(track: number): boolean;
  selectSubtitleTrack(track: number): boolean;
  snapshot(): PlayerSnapshot;
  progressSnapshot(): PlayerProgressSnapshot;
}

declare const qtavPlayer: QtAVPlayerNativeContext;

export default qtavPlayer;
