import { AbilityConstant, UIAbility, Want } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { window } from '@kit.ArkUI';

export default class EntryAbility extends UIAbility {
  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    hilog.info(0x0000, 'QtAVCoreOHOS', 'Ability created');
  }

  onDestroy(): void {
    hilog.info(0x0000, 'QtAVCoreOHOS', 'Ability destroyed');
  }

  onWindowStageCreate(windowStage: window.WindowStage): void {
    windowStage.loadContent('pages/Index', (error) => {
      if (error.code) {
        hilog.error(
          0x0000,
          'QtAVCoreOHOS',
          'Could not load the validation page: %{public}s',
          JSON.stringify(error) ?? 'unknown error');
      }
    });
  }

  onWindowStageDestroy(): void {
  }

  onForeground(): void {
  }

  onBackground(): void {
  }
}
