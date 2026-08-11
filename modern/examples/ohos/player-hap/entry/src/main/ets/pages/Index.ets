import { fileIo, picker } from '@kit.CoreFileKit';
import { PiPWindow, window } from '@kit.ArkUI';
import {
  PlayerProgressSnapshot,
  PlayerSnapshot,
  PlayerTrack,
  QtAVPlayerNativeContext
} from 'libqtav_player.so';

interface RateChoice {
  label: string;
  value: number;
}

const RATE_CHOICES: RateChoice[] = [
  { label: '0.5×', value: 0.5 },
  { label: '0.75×', value: 0.75 },
  { label: '1.0×', value: 1.0 },
  { label: '1.25×', value: 1.25 },
  { label: '1.5×', value: 1.5 },
  { label: '2.0×', value: 2.0 }
];

const RATE_OPTIONS: SelectOption[] = RATE_CHOICES.map(
  (choice: RateChoice): SelectOption => ({ value: choice.label }));
const DECODE_OPTIONS: SelectOption[] = [
  { value: '自动（优先 OHCodec）' },
  { value: '仅 OHCodec 硬解' },
  { value: '仅 FFmpeg 软解' }
];
const RENDER_OPTIONS: SelectOption[] = [
  { value: '自动（优先 Vulkan）' },
  { value: '仅 Vulkan' },
  { value: '仅 OpenGL ES' }
];
const HDR_OPTIONS: SelectOption[] = [
  { value: '自动（优先原生 HDR）' },
  { value: '必须原生 HDR' },
  { value: '强制 SDR tone-map' }
];

function emptySnapshot(): PlayerSnapshot {
  return {
    surfaceReady: false,
    playing: false,
    seekable: false,
    source: '',
    state: 'stopped',
    mediaStatus: 'no-media',
    subtitle: '',
    videoCodec: '',
    audioCodec: '',
    renderAPI: 'none',
    renderPreference: 'auto',
    decodePreference: 'auto',
    hdrPreference: 'auto',
    decoderAPI: 'pending',
    inputColorSpace: 'unknown',
    outputColorSpace: 'unknown',
    outputFormat: 'unknown',
    error: '',
    hdrInput: false,
    dolbyVisionInput: false,
    hdrOutput: false,
    toneMappedToSdr: false,
    positionMs: 0,
    durationMs: 0,
    playbackRate: 1.0,
    bufferingProgress: 1.0,
    fps: 0.0,
    width: 0,
    height: 0,
    activeAudioTrack: -1,
    activeSubtitleTrack: -1,
    decodedFrames: 0,
    hardwareFrames: 0,
    softwareFrames: 0,
    renderedFrames: 0,
    droppedFrames: 0,
    audioTracks: [],
    subtitleTracks: []
  };
}

@Entry
@Component
struct Index {
  @State snapshot: PlayerSnapshot = emptySnapshot();
  @State debugSnapshot: PlayerSnapshot = emptySnapshot();
  @State debugVisible: boolean = false;
  @State fullScreen: boolean = false;
  @State urlPanelVisible: boolean = false;
  @State urlText: string = '';
  @State seeking: boolean = false;
  @State sliderPosition: number = 0;
  @State subtitleText: string = '';
  @State pipActive: boolean = false;
  @State xComponentHdrBrightness: number = 1.0;
  @State notice: string = '打开本地文件或输入媒体 URL';
  @State audioSelectOptions: SelectOption[] = [{ value: '无音轨' }];
  @State subtitleSelectOptions: SelectOption[] = [{ value: '关闭字幕' }];

  private nativeContext: QtAVPlayerNativeContext | undefined = undefined;
  private xComponentController: XComponentController =
    new XComponentController();
  private pipController: PiPWindow.PiPController | undefined = undefined;
  private pipPlaying: boolean | undefined = undefined;
  private pipWidth: number = 0;
  private pipHeight: number = 0;
  private snapshotTimer: number = -1;
  private progressTimer: number = -1;
  private snapshotKey: string = '';

  private attachNativeContext(context: object): void {
    this.nativeContext = context as QtAVPlayerNativeContext;
    this.refreshSnapshot(true);
    this.refreshProgress();
    if (this.snapshotTimer < 0) {
      this.snapshotTimer = setInterval(() => {
        this.refreshSnapshot();
      }, 1000);
    }
    if (this.progressTimer < 0) {
      this.progressTimer = setInterval(() => {
        this.refreshProgress();
      }, 250);
    }
  }

  private refreshSnapshot(force: boolean = false): void {
    const nativeContext: QtAVPlayerNativeContext | undefined =
      this.nativeContext;
    if (!nativeContext) {
      return;
    }
    const next: PlayerSnapshot = nativeContext.snapshot();
    const key: string = this.controlSnapshotKey(next);
    if (force || key !== this.snapshotKey) {
      this.snapshot = next;
      this.snapshotKey = key;
      this.updateTrackOptions(next);
    }
    if (this.debugVisible) {
      this.debugSnapshot = next;
    }
    this.updatePiPControls(next);
  }

  private refreshProgress(): void {
    const nativeContext: QtAVPlayerNativeContext | undefined =
      this.nativeContext;
    if (!nativeContext) {
      return;
    }
    const next: PlayerProgressSnapshot = nativeContext.progressSnapshot();
    if (!this.seeking && this.sliderPosition !== next.positionMs) {
      this.sliderPosition = next.positionMs;
    }
    if (this.subtitleText !== next.subtitle) {
      this.subtitleText = next.subtitle;
    }
    let nextNotice: string = this.notice;
    if (next.error.length > 0) {
      nextNotice = next.error;
    } else if (next.mediaStatus === 'buffering') {
      nextNotice = `缓冲 ${Math.round(next.bufferingProgress * 100)}%`;
    } else if (this.snapshot.source.length > 0) {
      nextNotice = `${next.mediaStatus} · ${this.snapshot.source}`;
    }
    if (this.notice !== nextNotice) {
      this.notice = nextNotice;
    }
  }

  private controlSnapshotKey(value: PlayerSnapshot): string {
    const audioTracks: string = value.audioTracks.map(
      (track: PlayerTrack): string => `${track.index}:${track.label}`).join('|');
    const subtitleTracks: string = value.subtitleTracks.map(
      (track: PlayerTrack): string => `${track.index}:${track.label}`).join('|');
    return `${value.surfaceReady}|${value.playing}|${value.seekable}|` +
      `${value.source}|${value.state}|${value.mediaStatus}|${value.videoCodec}|` +
      `${value.audioCodec}|${value.renderAPI}|${value.renderPreference}|` +
      `${value.decodePreference}|${value.hdrPreference}|${value.decoderAPI}|` +
      `${value.inputColorSpace}|${value.outputColorSpace}|${value.outputFormat}|` +
      `${value.error}|${value.durationMs}|${value.playbackRate}|${value.width}|` +
      `${value.height}|${value.activeAudioTrack}|${value.activeSubtitleTrack}|` +
      `${value.hdrInput}|${value.dolbyVisionInput}|${value.hdrOutput}|` +
      `${audioTracks}|${subtitleTracks}`;
  }

  private updateTrackOptions(value: PlayerSnapshot): void {
    const audioOptions: SelectOption[] = [];
    value.audioTracks.forEach((track: PlayerTrack) => {
      audioOptions.push({ value: track.label });
    });
    if (audioOptions.length === 0) {
      audioOptions.push({ value: '无音轨' });
    }
    const subtitleOptions: SelectOption[] = [{ value: '关闭字幕' }];
    value.subtitleTracks.forEach((track: PlayerTrack) => {
      subtitleOptions.push({ value: track.label });
    });
    this.audioSelectOptions = audioOptions;
    this.subtitleSelectOptions = subtitleOptions;
  }

  private updatePiPControls(next: PlayerSnapshot): void {
    const controller = this.pipController;
    if (!controller) {
      return;
    }
    try {
      if (this.pipPlaying !== next.playing) {
        controller.updatePiPControlStatus(
          PiPWindow.PiPControlType.VIDEO_PLAY_PAUSE,
          next.playing ? PiPWindow.PiPControlStatus.PLAY :
            PiPWindow.PiPControlStatus.PAUSE);
        this.pipPlaying = next.playing;
      }
      if (next.width > 0 && next.height > 0 &&
        (this.pipWidth !== next.width || this.pipHeight !== next.height)) {
        controller.updateContentSize(next.width, next.height);
        this.pipWidth = next.width;
        this.pipHeight = next.height;
      }
    } catch {
    }
  }

  private async openLocalFile(): Promise<void> {
    const nativeContext: QtAVPlayerNativeContext | undefined =
      this.nativeContext;
    const hostContext = this.getUIContext().getHostContext();
    if (!nativeContext || !hostContext) {
      this.showToast('播放器尚未准备好');
      return;
    }
    try {
      const documentPicker = new picker.DocumentViewPicker(hostContext);
      const uris = await documentPicker.select({
        maxSelectNumber: 1,
        fileSuffixFilters: [
          '媒体文件|.mp4,.mkv,.mov,.m4v,.webm,.ts,.m2ts,.mp3,.m4a,.flac,.wav,.ogg,.opus'
        ]
      });
      if (uris.length === 0) {
        return;
      }
      const file = fileIo.openSync(uris[0], fileIo.OpenMode.READ_ONLY);
      let accepted = false;
      try {
        accepted = nativeContext.openLocalFd(
          file.fd,
          this.displayName(uris[0]));
      } finally {
        fileIo.closeSync(file);
      }
      if (!accepted) {
        this.showToast('无法打开选择的媒体文件');
      } else {
        this.notice = `正在打开 ${this.displayName(uris[0])}`;
      }
    } catch {
      this.showToast('本地文件选择已取消或打开失败');
    }
  }

  private openUrl(): void {
    const nativeContext: QtAVPlayerNativeContext | undefined =
      this.nativeContext;
    const url = this.urlText.trim();
    if (!nativeContext) {
      this.showToast('播放器尚未准备好');
      return;
    }
    if (url.length === 0) {
      this.showToast('请输入媒体 URL');
      return;
    }
    if (!nativeContext.openUrl(url)) {
      this.showToast('URL 未被播放器接受');
      return;
    }
    this.urlPanelVisible = false;
    this.notice = `正在打开 ${url}`;
  }

  private togglePlayback(): void {
    const nativeContext: QtAVPlayerNativeContext | undefined =
      this.nativeContext;
    if (!nativeContext || this.snapshot.source.length === 0) {
      this.showToast('请先打开媒体');
      return;
    }
    nativeContext.setPlaying(!this.snapshot.playing);
  }

  private stopPlayback(): void {
    this.nativeContext?.stop();
  }

  private seekTo(positionMs: number): void {
    if (!this.snapshot.seekable) {
      this.showToast('当前媒体不可定位');
      return;
    }
    this.nativeContext?.seek(Math.round(positionMs));
  }

  private seekRelative(deltaMs: number): void {
    const target = Math.max(
      0,
      Math.min(
        this.snapshot.durationMs,
        this.sliderPosition + deltaMs));
    this.seekTo(target);
  }

  private setRate(index: number): void {
    if (index < 0 || index >= RATE_CHOICES.length) {
      return;
    }
    if (!this.nativeContext?.setRate(RATE_CHOICES[index].value)) {
      this.showToast('无法设置该播放速度');
    }
  }

  private setDecodeMode(index: number): void {
    const values: string[] = ['auto', 'hardware', 'software'];
    if (index < 0 || index >= values.length ||
      !this.nativeContext?.setDecodeMode(values[index])) {
      this.showToast('无法切换解码模式');
      return;
    }
    this.refreshSnapshot(true);
  }

  private setRenderMode(index: number): void {
    const values: string[] = ['auto', 'vulkan', 'opengles'];
    if (index < 0 || index >= values.length ||
      !this.nativeContext?.setRenderMode(values[index])) {
      this.showToast('无法切换渲染后端');
      return;
    }
    this.refreshSnapshot(true);
  }

  private setHdrMode(index: number): void {
    const values: string[] = ['auto', 'require-hdr', 'sdr'];
    if (index < 0 || index >= values.length ||
      !this.nativeContext?.setHdrMode(values[index])) {
      this.showToast('无法切换 HDR 输出模式');
      return;
    }
    // A SURFACE XComponent has a separate RenderService HDR-light-up hint.
    // Keep it aligned with the native Vulkan/EGL output policy: automatic
    // and required-HDR modes permit the panel's current HDR peak, while the
    // explicit SDR mode asks ArkUI to compose the surface at SDR brightness.
    this.xComponentHdrBrightness = index === 2 ? 0.0 : 1.0;
    this.refreshSnapshot(true);
  }

  private selectAudio(index: number): void {
    if (index < 0 || index >= this.snapshot.audioTracks.length) {
      return;
    }
    const track: PlayerTrack = this.snapshot.audioTracks[index];
    if (!this.nativeContext?.selectAudioTrack(track.index)) {
      this.showToast('音轨切换请求失败');
    }
  }

  private selectSubtitle(index: number): void {
    if (index === 0) {
      if (!this.nativeContext?.selectSubtitleTrack(-1)) {
        this.showToast('无法关闭字幕');
      }
      return;
    }
    const trackIndex = index - 1;
    if (trackIndex < 0 || trackIndex >= this.snapshot.subtitleTracks.length) {
      return;
    }
    if (!this.nativeContext?.selectSubtitleTrack(
      this.snapshot.subtitleTracks[trackIndex].index)) {
      this.showToast('字幕切换请求失败');
    }
  }

  private async toggleFullScreen(): Promise<void> {
    const hostContext = this.getUIContext().getHostContext();
    if (!hostContext) {
      this.showToast('无法取得主窗口');
      return;
    }
    const next = !this.fullScreen;
    try {
      const mainWindow = await window.getLastWindow(hostContext);
      await mainWindow.setWindowLayoutFullScreen(next);
      await mainWindow.setWindowSystemBarEnable(
        next ? [] : ['status', 'navigation']);
      await mainWindow.setPreferredOrientation(
        next ? window.Orientation.AUTO_ROTATION_LANDSCAPE :
          window.Orientation.AUTO_ROTATION);
      this.fullScreen = next;
    } catch {
      this.showToast('无法切换全屏模式');
    }
  }

  private async startPiP(): Promise<void> {
    if (!PiPWindow.isPiPEnabled()) {
      this.showToast('当前设备未启用画中画');
      return;
    }
    const hostContext = this.getUIContext().getHostContext();
    if (!hostContext || this.snapshot.source.length === 0) {
      this.showToast('请先打开媒体');
      return;
    }
    try {
      if (!this.pipController) {
        const controller = await PiPWindow.create({
          context: hostContext,
          componentController: this.xComponentController,
          templateType: PiPWindow.PiPTemplateType.VIDEO_PLAY,
          contentWidth: this.snapshot.width > 0 ? this.snapshot.width : 16,
          contentHeight: this.snapshot.height > 0 ? this.snapshot.height : 9,
          controlGroups: [
            PiPWindow.VideoPlayControlGroup.FAST_FORWARD_BACKWARD
          ]
        });
        controller.on(
          'stateChange',
          (state: PiPWindow.PiPState, reason: string) => {
            this.pipActive = state === PiPWindow.PiPState.STARTED ||
              state === PiPWindow.PiPState.ABOUT_TO_START;
            if (state === PiPWindow.PiPState.ERROR) {
              this.notice = `画中画错误：${reason}`;
            }
          });
        controller.on(
          'controlEvent',
          (event: PiPWindow.ControlEventParam) => {
            if (event.controlType ===
              PiPWindow.PiPControlType.VIDEO_PLAY_PAUSE) {
              this.nativeContext?.setPlaying(
                event.status === PiPWindow.PiPControlStatus.PLAY);
            } else if (event.controlType ===
              PiPWindow.PiPControlType.FAST_FORWARD) {
              this.seekRelative(10_000);
            } else if (event.controlType ===
              PiPWindow.PiPControlType.FAST_BACKWARD) {
              this.seekRelative(-10_000);
            }
          });
        controller.setAutoStartEnabled(true);
        this.pipController = controller;
        this.pipPlaying = undefined;
        this.pipWidth = 0;
        this.pipHeight = 0;
      }
      await this.pipController.startPiP();
    } catch {
      this.showToast('启动画中画失败');
    }
  }

  private async togglePiP(): Promise<void> {
    if (!this.pipActive) {
      await this.startPiP();
      return;
    }
    try {
      await this.pipController?.stopPiP();
    } catch {
      this.showToast('退出画中画失败');
    }
  }

  private displayName(uri: string): string {
    const parts = uri.split('/');
    return parts.length > 0 ? parts[parts.length - 1] : 'Local file';
  }

  private showToast(message: string): void {
    this.notice = message;
    try {
      this.getUIContext().getPromptAction().showToast({ message: message });
    } catch {
    }
  }

  private formatTime(milliseconds: number): string {
    const totalSeconds = Math.max(0, Math.floor(milliseconds / 1000));
    const hours = Math.floor(totalSeconds / 3600);
    const minutes = Math.floor((totalSeconds % 3600) / 60);
    const seconds = totalSeconds % 60;
    const minuteText = hours > 0 && minutes < 10 ? `0${minutes}` :
      `${minutes}`;
    const secondText = seconds < 10 ? `0${seconds}` : `${seconds}`;
    return hours > 0 ? `${hours}:${minuteText}:${secondText}` :
      `${minuteText}:${secondText}`;
  }

  private rateSelection(): number {
    for (let index = 0; index < RATE_CHOICES.length; ++index) {
      if (Math.abs(
        RATE_CHOICES[index].value - this.snapshot.playbackRate) < 0.01) {
        return index;
      }
    }
    return 2;
  }

  private decodeSelection(): number {
    if (this.snapshot.decodePreference === 'hardware') {
      return 1;
    }
    return this.snapshot.decodePreference === 'software' ? 2 : 0;
  }

  private renderSelection(): number {
    if (this.snapshot.renderPreference === 'vulkan') {
      return 1;
    }
    return this.snapshot.renderPreference === 'opengles' ? 2 : 0;
  }

  private hdrSelection(): number {
    if (this.snapshot.hdrPreference === 'require-hdr') {
      return 1;
    }
    return this.snapshot.hdrPreference === 'sdr' ? 2 : 0;
  }

  private audioSelection(): number {
    for (let index = 0; index < this.snapshot.audioTracks.length; ++index) {
      if (this.snapshot.audioTracks[index].index ===
        this.snapshot.activeAudioTrack) {
        return index;
      }
    }
    return 0;
  }

  private subtitleSelection(): number {
    if (this.snapshot.activeSubtitleTrack < 0) {
      return 0;
    }
    for (let index = 0; index < this.snapshot.subtitleTracks.length; ++index) {
      if (this.snapshot.subtitleTracks[index].index ===
        this.snapshot.activeSubtitleTrack) {
        return index + 1;
      }
    }
    return 0;
  }

  private debugText(): string {
    const info: PlayerSnapshot = this.debugSnapshot;
    const dimensions: string = info.width > 0 && info.height > 0 ?
      `${info.width}×${info.height}` : '等待视频';
    const videoCodec: string = info.videoCodec.length > 0 ?
      info.videoCodec : '-';
    const audioCodec: string = info.audioCodec.length > 0 ?
      info.audioCodec : '-';
    const source: string = info.source.length > 0 ?
      info.source : '未打开媒体';
    const inputHdr: string = info.dolbyVisionInput ? 'Dolby Vision' :
      (info.hdrInput ? `HDR ${info.inputColorSpace}` : `SDR ${info.inputColorSpace}`);
    const outputHdr: string = info.hdrOutput ?
      `HDR ${info.outputColorSpace} ${info.outputFormat}` :
      `SDR ${info.outputColorSpace} ${info.outputFormat}`;
    const conversion: string = info.toneMappedToSdr ? ' · HDR→SDR tone-map' : '';
    return `${source}\n` +
      `${dimensions} · V ${videoCodec} · A ${audioCodec}\n` +
      `${info.decoderAPI}(${info.decodePreference}) · ${info.renderAPI}(${info.renderPreference})\n` +
      `输入 ${inputHdr} · 输出 ${outputHdr}${conversion} · XComponent HDR ${this.xComponentHdrBrightness.toFixed(1)}\n` +
      `${info.mediaStatus} · ${info.playbackRate.toFixed(2)}× · FPS ${info.fps.toFixed(1)}\n` +
      `硬解帧 ${info.hardwareFrames} · 软解帧 ${info.softwareFrames} · 呈现 ${info.renderedFrames}\n` +
      `丢帧 ${info.droppedFrames} · 音轨 ${info.activeAudioTrack} · 字幕 ${info.activeSubtitleTrack}`;
  }

  aboutToDisappear(): void {
    if (this.snapshotTimer >= 0) {
      clearInterval(this.snapshotTimer);
      this.snapshotTimer = -1;
    }
    if (this.progressTimer >= 0) {
      clearInterval(this.progressTimer);
      this.progressTimer = -1;
    }
    this.nativeContext = undefined;
  }

  @Builder
  private videoPane() {
    Stack({ alignContent: Alignment.TopStart }) {
      XComponent({
        id: 'qtav-player-video',
        type: XComponentType.SURFACE,
        libraryname: 'qtav_player',
        controller: this.xComponentController
      })
        .width('100%')
        .height('100%')
        .backgroundColor(Color.Black)
        .hdrBrightness(this.xComponentHdrBrightness)
        .onLoad((context: object) => {
          this.attachNativeContext(context);
        })

      if (this.debugVisible) {
        Column() {
          Row() {
            Text('媒体信息')
              .fontSize(13)
              .fontWeight(FontWeight.Bold)
              .fontColor(Color.White)
              .layoutWeight(1)
            Text('X')
              .width(30)
              .height(30)
              .fontSize(18)
              .fontWeight(FontWeight.Bold)
              .fontColor(Color.White)
              .textAlign(TextAlign.Center)
              .borderRadius(15)
              .backgroundColor('#33404e')
              .onClick(() => {
                this.debugVisible = false;
              })
          }
          .width('100%')

          Text(this.debugText())
            .fontSize(11)
            .lineHeight(16)
            .fontColor('#f2f4f8')
            .maxLines(8)
            .textOverflow({ overflow: TextOverflow.Ellipsis })
        }
        .width(this.fullScreen ? 360 : '88%')
        .padding({ left: 10, right: 8, top: 4, bottom: 8 })
        .margin({ left: 12, top: 12 })
        .borderRadius(8)
        .backgroundColor('#b81a1d24')
      }

      if (this.subtitleText.length > 0) {
        Column() {
          Blank()
            .layoutWeight(1)
          Text(this.subtitleText)
            .fontSize(this.fullScreen ? 24 : 18)
            .fontColor(Color.White)
            .fontWeight(FontWeight.Medium)
            .textAlign(TextAlign.Center)
            .padding({ left: 10, right: 10, top: 4, bottom: 4 })
            .margin({
              left: 20,
              right: 20,
              bottom: this.fullScreen ? 96 : 18
            })
            .borderRadius(4)
            .backgroundColor('#99000000')
        }
        .width('100%')
        .height('100%')
      }
    }
    .width('100%')
    .height('100%')
    .backgroundColor(Color.Black)
  }

  @Builder
  private progressControls() {
    Row() {
      Text(this.formatTime(this.sliderPosition))
        .width(58)
        .fontSize(12)
        .fontColor(Color.White)
      Slider({
        value: this.sliderPosition,
        min: 0,
        max: Math.max(1, this.snapshot.durationMs),
        step: 100
      })
        .layoutWeight(1)
        .enabled(this.snapshot.seekable)
        .selectedColor('#4d8dff')
        .trackColor('#647080')
        .onChange((value: number, mode: SliderChangeMode) => {
          this.sliderPosition = value;
          this.seeking = mode !== SliderChangeMode.End;
          if (mode === SliderChangeMode.End) {
            this.seekTo(value);
            this.seeking = false;
          }
        })
      Text(this.formatTime(this.snapshot.durationMs))
        .width(58)
        .fontSize(12)
        .fontColor(Color.White)
        .textAlign(TextAlign.End)
    }
    .width('100%')
  }

  @Builder
  private primaryControls(compact: boolean) {
    Flex({
      direction: FlexDirection.Row,
      wrap: FlexWrap.Wrap,
      justifyContent: FlexAlign.Center,
      alignItems: ItemAlign.Center
    }) {
      Button(this.snapshot.playing ? '暂停' : '播放')
        .height(38)
        .margin(4)
        .onClick(() => this.togglePlayback())
      Button('停止')
        .height(38)
        .margin(4)
        .onClick(() => this.stopPlayback())
      Button('−10 秒')
        .height(38)
        .margin(4)
        .enabled(this.snapshot.seekable)
        .onClick(() => this.seekRelative(-10_000))
      Button('+10 秒')
        .height(38)
        .margin(4)
        .enabled(this.snapshot.seekable)
        .onClick(() => this.seekRelative(10_000))
      Select(RATE_OPTIONS)
        .selected(this.rateSelection())
        .value('倍速')
        .height(38)
        .margin(4)
        .fontColor('#f2f4f8')
        .backgroundColor('#263040')
        .onSelect((index: number) => this.setRate(index))
      Button(this.debugVisible ? '关闭信息' : '媒体信息')
        .height(38)
        .margin(4)
        .onClick(() => {
          this.debugVisible = !this.debugVisible;
          if (this.debugVisible) {
            this.refreshSnapshot(true);
          }
        })
      Button(this.fullScreen ? '退出全屏' : '全屏')
        .height(38)
        .margin(4)
        .onClick(() => this.toggleFullScreen())
      Button(this.pipActive ? '退出画中画' : '画中画')
        .height(38)
        .margin(4)
        .onClick(() => this.togglePiP())
    }
    .width('100%')
    .padding({ left: compact ? 12 : 4, right: compact ? 12 : 4 })
  }

  @Builder
  private trackControls() {
    Flex({
      direction: FlexDirection.Row,
      wrap: FlexWrap.Wrap,
      justifyContent: FlexAlign.Center,
      alignItems: ItemAlign.Center
    }) {
      Text('音轨')
        .fontSize(13)
        .fontColor('#cbd3df')
        .margin({ left: 6, right: 4 })
      Select(this.audioSelectOptions)
        .selected(this.audioSelection())
        .value('选择音轨')
        .enabled(this.snapshot.audioTracks.length > 0)
        .width(190)
        .margin(4)
        .fontColor('#f2f4f8')
        .backgroundColor('#263040')
        .onSelect((index: number) => this.selectAudio(index))
      Text('字幕')
        .fontSize(13)
        .fontColor('#cbd3df')
        .margin({ left: 6, right: 4 })
      Select(this.subtitleSelectOptions)
        .selected(this.subtitleSelection())
        .value('选择字幕')
        .width(190)
        .margin(4)
        .fontColor('#f2f4f8')
        .backgroundColor('#263040')
        .onSelect((index: number) => this.selectSubtitle(index))
    }
    .width('100%')
  }

  @Builder
  private pipelineControls() {
    Flex({
      direction: FlexDirection.Row,
      wrap: FlexWrap.Wrap,
      justifyContent: FlexAlign.Center,
      alignItems: ItemAlign.Center
    }) {
      Text('解码')
        .fontSize(13)
        .fontColor('#cbd3df')
        .margin({ left: 6, right: 4 })
      Select(DECODE_OPTIONS)
        .selected(this.decodeSelection())
        .value(DECODE_OPTIONS[this.decodeSelection()].value)
        .width(190)
        .margin(4)
        .fontColor('#f2f4f8')
        .backgroundColor('#263040')
        .onSelect((index: number) => this.setDecodeMode(index))
      Text('渲染')
        .fontSize(13)
        .fontColor('#cbd3df')
        .margin({ left: 6, right: 4 })
      Select(RENDER_OPTIONS)
        .selected(this.renderSelection())
        .value(RENDER_OPTIONS[this.renderSelection()].value)
        .width(190)
        .margin(4)
        .fontColor('#f2f4f8')
        .backgroundColor('#263040')
        .onSelect((index: number) => this.setRenderMode(index))
      Text('HDR')
        .fontSize(13)
        .fontColor('#cbd3df')
        .margin({ left: 6, right: 4 })
      Select(HDR_OPTIONS)
        .selected(this.hdrSelection())
        .value(HDR_OPTIONS[this.hdrSelection()].value)
        .width(190)
        .margin(4)
        .fontColor('#f2f4f8')
        .backgroundColor('#263040')
        .onSelect((index: number) => this.setHdrMode(index))
    }
    .width('100%')
  }

  @Builder
  private normalControls() {
    Column() {
      this.progressControls()
      this.primaryControls(false)
      this.pipelineControls()
      this.trackControls()

      Flex({
        direction: FlexDirection.Row,
        wrap: FlexWrap.Wrap,
        justifyContent: FlexAlign.Center
      }) {
        Button('打开本地文件')
          .height(40)
          .margin(4)
          .onClick(() => this.openLocalFile())
        Button(this.urlPanelVisible ? '收起 URL' : '打开 URL')
          .height(40)
          .margin(4)
          .onClick(() => {
            this.urlPanelVisible = !this.urlPanelVisible;
          })
      }
      .width('100%')

      if (this.urlPanelVisible) {
        Row() {
          TextInput({
            text: this.urlText,
            placeholder: 'https://example.com/video.mp4'
          })
            .layoutWeight(1)
            .height(44)
            .onChange((value: string) => {
              this.urlText = value;
            })
          Button('播放 URL')
            .height(40)
            .margin({ left: 8 })
            .onClick(() => this.openUrl())
        }
        .width('100%')
        .padding({ left: 12, right: 12, top: 4, bottom: 4 })
      }

      Text(this.notice)
        .width('100%')
        .fontSize(12)
        .fontColor(this.snapshot.error.length > 0 ? '#ff8a80' : '#9aa7b8')
        .maxLines(2)
        .textOverflow({ overflow: TextOverflow.Ellipsis })
        .padding({ left: 12, right: 12, top: 4, bottom: 8 })
    }
    .width('100%')
    .backgroundColor('#151922')
  }

  build() {
    Column() {
      if (this.fullScreen) {
        Stack({ alignContent: Alignment.Bottom }) {
          this.videoPane()
          Column() {
            this.progressControls()
            this.primaryControls(true)
          }
          .width('100%')
          .padding({ left: 14, right: 14, top: 8, bottom: 10 })
          .backgroundColor('#b0000000')
        }
        .width('100%')
        .height('100%')
      } else {
        Stack() {
          this.videoPane()
        }
        .width('100%')
        .height('44%')
        Scroll() {
          this.normalControls()
        }
        .width('100%')
        .layoutWeight(1)
        .scrollBar(BarState.Auto)
      }
    }
    .width('100%')
    .height('100%')
    .backgroundColor('#0b0d12')
  }
}
