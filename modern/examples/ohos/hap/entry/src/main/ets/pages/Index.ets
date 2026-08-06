interface QtAVNativeContext {
  start(h264Media: Uint8Array, hevcMedia: Uint8Array): boolean;
  setForeground(foreground: boolean): void;
  stop(): void;
  status(): string;
}

@Entry
@Component
struct Index {
  @State statusText: string = 'Waiting for the XComponent surface';
  @State surfaceVisible: boolean = true;
  private nativeContext: QtAVNativeContext | undefined = undefined;
  private statusTimer: number = -1;
  private mediaLoadStarted: boolean = false;
  private pageForeground: boolean = true;
  private foregroundReportPending: boolean = true;
  private disposed: boolean = false;

  private attachNativeContext(context: object): void {
    this.nativeContext = context as QtAVNativeContext;
    if (this.foregroundReportPending) {
      this.nativeContext.setForeground(this.pageForeground);
      this.foregroundReportPending = false;
    }
    if (this.statusTimer < 0) {
      this.statusTimer = setInterval(() => {
        if (this.nativeContext) {
          this.statusText = this.nativeContext.status();
        }
      }, 250);
    }
    if (this.mediaLoadStarted) {
      return;
    }
    this.mediaLoadStarted = true;
    const hostContext = this.getUIContext().getHostContext();
    if (!hostContext) {
      this.statusText = 'Could not obtain the ArkUI host context';
      return;
    }
    Promise.all([
      hostContext.resourceManager.getRawFileContent(
        'qtav-ohos-test-h264.mp4'),
      hostContext.resourceManager.getRawFileContent(
        'qtav-ohos-test-hevc.mp4')
    ])
      .then((media: Uint8Array[]) => {
        if (this.disposed) {
          return;
        }
        const nativeContext = this.nativeContext;
        if (!nativeContext || !nativeContext.start(media[0], media[1])) {
          this.statusText = 'Native start rejected the packaged H.264/HEVC media';
          return;
        }
      })
      .catch((error: Error) => {
        if (!this.disposed) {
          this.statusText = `Could not load packaged H.264/HEVC media: ${error.message}`;
        }
      });
  }

  private reportPageForeground(foreground: boolean): void {
    this.pageForeground = foreground;
    if (this.nativeContext) {
      this.nativeContext.setForeground(foreground);
      this.foregroundReportPending = false;
    } else {
      this.foregroundReportPending = true;
    }
  }

  onPageHide(): void {
    this.reportPageForeground(false);
    this.surfaceVisible = false;
  }

  onPageShow(): void {
    this.surfaceVisible = true;
    this.reportPageForeground(true);
  }

  aboutToDisappear(): void {
    this.disposed = true;
    if (this.statusTimer >= 0) {
      clearInterval(this.statusTimer);
      this.statusTimer = -1;
    }
    this.nativeContext = undefined;
  }

  build() {
    Column() {
      if (this.surfaceVisible) {
        XComponent({
          id: 'qtav-video',
          type: XComponentType.SURFACE,
          libraryname: 'entry'
        })
          .width('100%')
          .layoutWeight(1)
          .backgroundColor(Color.Black)
          .onLoad((context: object) => {
            this.attachNativeContext(context);
          })
      } else {
        Blank()
          .width('100%')
          .layoutWeight(1)
          .backgroundColor(Color.Black)
      }
      Text(this.statusText)
        .width('100%')
        .padding(12)
        .fontSize(14)
        .fontColor(Color.White)
        .backgroundColor('#202020')
    }
    .width('100%')
    .height('100%')
    .backgroundColor(Color.Black)
  }
}
