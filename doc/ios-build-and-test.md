# Machinedrum on your iPhone

Open `build/ios-phone-signed/gearmulator.xcodeproj` in Xcode.

1. Select the `mdJucePlugin_Standalone` scheme and **Michael Meneses’s iPhone** as the destination.
2. Keep the iPhone connected and unlocked. Accept any trust or developer-app approval on the phone.
3. Click Run (Command-R). The scheme uses the optimized **Release** configuration.
4. If macOS asks to let codesign use the Apple Development key, approve it using your Mac login password. The P12 export password is not the Mac login password.
5. Once the app launches, check that the Machinedrum panel appears, start playback, and listen for audio. Device audio performance and two-handed touch behavior still need testing.

Signing is configured for **Michael Meneses (Personal Team)**, team `MZZ78TC6YL`, with automatic signing and app ID `com.michaelmeneses.mzz78tc6yl.GearmulatorMD`. The local ROM is included in this personal build. No paid developer enrollment or public upload is part of this setup.

If signing needs attention, select target `mdJucePlugin_Standalone` → Signing & Capabilities, confirm automatic signing and the Personal Team, and use Try Again with the phone selected. Read the actual error before changing any certificates: the development signing identity has already been installed and verified.

## Command-line build

```sh
cd /Users/michaelmeneses/gearmulator-md-mm
xcodebuild -project build/ios-phone-signed/gearmulator.xcodeproj \
  -scheme mdJucePlugin_Standalone -configuration Release \
  -destination 'id=00008110-001035420EF1401E' \
  -allowProvisioningUpdates -allowProvisioningDeviceRegistration -jobs 4 build
```

Do not rebuild the macOS standalone into the shared `bin/plugins/Release/Standalone` folder while preparing this iOS build: both currently use that output location.

For detailed setup evidence, outstanding issues and Hermes continuation, see `hermes-ios-handoff.md`.
