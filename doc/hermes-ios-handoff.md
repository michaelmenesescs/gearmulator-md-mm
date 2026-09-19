# Machinedrum iPhone handoff — 2026-09-19

User explicitly requested completing the Machinedrum iPhone installation and handing off to Hermes. Codex is actively finishing the build/install. For this initial handoff, inspect only, acknowledge receipt, and suggest any missed solution to Xcode's destination/provisioning issue. Do not launch competing builds, alter signing, restart services, or modify files while Codex is working. Do not message external channels.

Repository: `/Users/michaelmeneses/gearmulator-md-mm` (dirty worktree, preserve all changes). Old `doc/ios-machinedrum-plan.md` has stale blocker text: full Xcode IS installed.

## Confirmed progress

- Xcode 26.3 (17C529) at `/Applications/Xcode.app`.
- User supplied a valid Apple Development P12. Imported to login keychain successfully outside the Codex sandbox. The earlier import errors were sandbox-related; no private key extraction was needed.
- Imported official Apple WWDR G3 intermediate from `https://www.apple.com/certificateauthority/AppleWWDRCAG3.cer`. `security find-identity -v -p codesigning` now returns valid identity `058AC31ED7D1D926954249D32BE5AB7728FEF71F`.
- Personal Team `MZZ78TC6YL`.
- iPhone 14, UDID `00008110-001035420EF1401E`, CoreDevice ID `A68C202E-24AD-564E-AA82-6CC8522E45E0`.
- `devicectl device info details` confirms paired, wired, connected, Developer Mode enabled, DDI services available, iOS 26.6.2.
- CMake Xcode generation works outside sandbox: `build/ios-phone-signed/gearmulator.xcodeproj`. Target/scheme `mdJucePlugin_Standalone`, Release, arm64, deployment minimum 14.0, interpreter-only.
- Added configurable `GEARMULATOR_BUNDLE_ID_PREFIX` in `source/juce.cmake`, preserving desktop default. Personal build uses `com.michaelmeneses.mzz78tc6yl`, hence bundle ID `com.michaelmeneses.mzz78tc6yl.GearmulatorMD`. Original `local.gearmulator.preview.GearmulatorMD` was rejected as unavailable by Apple.
- Prior source fix excludes iOS from anonymous desktop post-build ad-hoc signing.
- ROM private local path `/Users/michaelmeneses/Documents/Gearmulator Preview/Machinedrum/roms/elektron_sps1-1uw_os1.63.bin`; CMake `GEARMULATOR_IOS_ROM` includes it in this explicitly authorized personal app. Never publish/upload ROM or signing secrets.

## Current blockers and running work

- Scheme device build says `iOS 26.2 is not installed` for BOTH connected phone and generic iOS destination, although `xcodebuild -showsdks` lists iphoneos26.2 and direct target builds reach provisioning.
- Direct target automatic signing reaches Apple but says team has no devices to generate provisioning profile. `-destination id=...` on target invocation does not fix this. Scheme destination must become eligible to register device.
- `xcodebuild -runFirstLaunch -checkForNewerComponents` installed XcodeSystemResources.pkg successfully. CoreSimulator version changed from 1051.17.8 to 1051.17.11 during that command; subsequent processes use new version.
- `xcodebuild -downloadPlatform iOS -buildVersion 26.2` unavailable. Running `xcodebuild -downloadPlatform iOS` selected 26.3.1 arm64 simulator, 8.39 GB. Log `/private/tmp/machinedrum-platform-download.log`. Internal disk initially had about 15 GiB free; monitor capacity.
- Codex compiling Release app with `CODE_SIGNING_ALLOWED=NO` in parallel, log `/private/tmp/machinedrum-iphone-compile.log`. This does NOT produce an installable signature; signing/profile must follow.
- Signed build log `/private/tmp/machinedrum-iphone-build.log`; config log `/private/tmp/machinedrum-iphone-configure.log`.

## Commands once platform/device eligibility is resolved

```sh
cd /Users/michaelmeneses/gearmulator-md-mm
xcodebuild -project build/ios-phone-signed/gearmulator.xcodeproj -scheme mdJucePlugin_Standalone -configuration Release -destination 'id=00008110-001035420EF1401E' -allowProvisioningUpdates -allowProvisioningDeviceRegistration -jobs 4 build
xcrun devicectl device install app --device A68C202E-24AD-564E-AA82-6CC8522E45E0 'bin/plugins/Release/Standalone/Gearmulator MD.app'
xcrun devicectl device process launch --device A68C202E-24AD-564E-AA82-6CC8522E45E0 com.michaelmeneses.mzz78tc6yl.GearmulatorMD
```

Verify signature, embedded provisioning profile and phone UDID, bundled ROM checksum, actual install result and launch. Do not claim audio/UI/two-handed touch testing without evidence. iPad target family 1,2 is configured but no iPad hardware test has occurred. Private ROM and signing credentials must stay local. Await Codex's final status update before taking ownership of mutations.
