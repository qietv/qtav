# Temporary Windows-only CI record

Date: 2026-08-10

This record preserves the first published CI result and the temporary
Windows-only scope selected after that run. The active status remains in
[`PLAN.md`](PLAN.md), and the operational contract remains in [`CI.md`](CI.md).

## First published run

GitHub Actions [run `31378081488`](https://github.com/qietv/qtav/actions/runs/31378081488)
executed commit
`84d8470523279317c83aa25fa54e62c7595ab1e1` on the configured Windows
self-hosted runner. All three jobs completed with failures:

- Android stopped before its build because the runner service resolved the SDK
  under the `NetworkService` profile and found no `sdkmanager.bat`;
- OHOS stopped before its build because the runner service could not discover
  an OpenHarmony native SDK;
- Windows built the shared package and passed 20 of 21 configured tests, but
  `qtav_render_d3d11_advanced_color` timed out after 15 seconds in the
  non-interactive runner service session.

The run is failure evidence, not a supported-target CI pass.

## Temporary scope

By explicit project direction, the GitHub Actions workflow now contains only
the Windows shared/static Release build, CTest, install, and package-consumer
job. Android and OHOS Actions jobs are removed temporarily. Their checked-in
PowerShell drivers, local cross-build evidence, target support, and separate
physical-device gates remain unchanged.

The Windows Advanced Color test now returns the repository CTest skip code 77
before D3D resource creation when the process has no interactive window
station. This prevents a service-session display probe from hanging without
claiming that native display/HDR validation passed.

## Local validation

From the repository root, the Windows runner-equivalent driver completed with
the existing dependency prefix after re-running its install verifier:

```powershell
./modern/scripts/ci/build-windows.ps1 -SkipDependencies -Parallel 8
```

The shared and static Release configurations each passed 60 of 60 CTest tests,
installed their packages, and completed the staged installed-package consumer.
The Advanced Color test passed on the interactive development desktop in both
configurations, in 0.57 seconds for shared and 0.52 seconds for static.

## Published Windows-only pass

GitHub Actions [run `31383223536`](https://github.com/qietv/qtav/actions/runs/31383223536)
executed commit
`7e81a0f1b655ff0c34673fe8b8222ede582ab717`. Its only job was the Windows
shared/static Release gate. Each configuration completed all 21 tests without
failure, with the non-interactive Advanced Color test explicitly skipped,
then installed the package and completed the staged installed-package
consumer. The run uploaded the bounded `qtavcore-windows-ci-logs` artifact.

The successful run also exposed a non-fatal cache post-step warning: Git GNU
tar could not find its adjacent `gzip.exe` through the service runner's PATH.
The workflow now adds that Git `usr/bin` directory to `GITHUB_PATH` after the
build so subsequent cache saves can resolve the compressor.

## Completion status

The temporary Windows-only build, test, install, and package-consumer task is
complete. Restoring Android/OHOS Actions execution remains a separate
incomplete gate that must not resume without explicit project direction.
