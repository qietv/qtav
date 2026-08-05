interface QtAVNativeContext {
  start(media: Uint8Array): boolean;
  stop(): void;
  status(): string;
}

@Entry
@Component
struct Index {
  @State statusText: string = 'Waiting for the XComponent surface';
  private nativeContext: QtAVNativeContext | undefined = undefined;
  private statusTimer: number = -1;

  private startPlayback(context: object): void {
    this.nativeContext = context as QtAVNativeContext;
    const hostContext = this.getUIContext().getHostContext();
    if (!hostContext) {
      this.statusText = 'Could not obtain the ArkUI host context';
      return;
    }
    hostContext.resourceManager
      .getRawFileContent('qtav-ohos-test.mp4')
      .then((media: Uint8Array) => {
        if (!this.nativeContext?.start(media)) {
          this.statusText = 'Native start rejected the packaged media';
          return;
        }
        this.statusTimer = setInterval(() => {
          if (this.nativeContext) {
            this.statusText = this.nativeContext.status();
          }
        }, 250);
      })
      .catch((error: Error) => {
        this.statusText = `Could not load packaged media: ${error.message}`;
      });
  }

  aboutToDisappear(): void {
    if (this.statusTimer >= 0) {
      clearInterval(this.statusTimer);
      this.statusTimer = -1;
    }
  }

  build() {
    Column() {
      XComponent({
        id: 'qtav-video',
        type: XComponentType.SURFACE,
        libraryname: 'entry'
      })
        .width('100%')
        .layoutWeight(1)
        .backgroundColor(Color.Black)
        .onLoad((context: object) => {
          this.startPlayback(context);
        })
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
