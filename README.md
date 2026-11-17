# StrideXR 1.6.3

StrideXR provides arm-swing locomotion, a VR test scene, and an MP3 player with optional subtitles on Windows OpenXR. It is designed for Quest Touch controllers and Virtual Desktop's VDXR runtime.

## Download and start

[Download the latest Windows release](https://github.com/page-muncher/StrideXR/releases/latest).

Download `StrideXR-1.6.3.exe` and run it on Windows x64. Check **Enabled** to install and register the 32-bit and 64-bit OpenXR layers for your Windows user. A Start menu shortcut is created. Connect your Quest through Virtual Desktop with VDXR selected, choose or create a game profile, then launch the game as usual. Restart a running VR game after installing an update.

Use the **Audiobook** tab to select an MP3 and optional SRT before starting Fallout. Use **VR Lab** with the game closed to try movement and adjust the subtitle panel. Settings save under `%LOCALAPPDATA%\StrideXR` and are kept across updates.

The release ZIP packages the same executable with the guide and dependency license notices. Source and SHA-256 checksums are available on the release page.

## Audiobooks in TTWVR

1. Open StrideXR's **Audiobook** tab. Select an MP3. A same-named SRT next to it is loaded automatically. Use the SRT **Browse** button for a different file.
2. Leave **Enable audiobook in Fallout** and StrideXR's **Enabled** box checked. Keep StrideXR open and start Tale of Two Wastelands through Mod Organizer as usual. Fully restart Fallout after installing this version.
3. Tap the left Quest Touch controller's **Menu** button to play or pause. The subtitle panel follows your view near the top center. Its empty highlight box stays visible between cues. Adjust playback position and volume from the Audiobook tab.

StrideXR opens the last selected book paused at its saved position. The audiobook feature targets TTWVR's 32-bit D3D11 OpenXR session in `FalloutNV.exe` and works independently of its swing profile.

MP3s loop automatically at the end. The SRT is optional. Select **Clear SRT** to switch to audio-only playback. After you pause playback, the subtitle display starts fading after five seconds and returns when you resume.

MP3 seeking uses an index of decoded samples. Subtitles follow the audio output's sample counter throughout the book. Use **Delay (s)** to correct a fixed difference in the source SRT. Positive values display it later. **Box lines** sets the maximum number of highlighted rows. The first highlighted row starts at a fixed spot, and the empty box stays visible during gaps. The selected layout shows up to two dimmed rows above or below the box on each enabled side. **History** updates in place and shows completed lines above the current cue. Longer cues are spread over their original cue time.

## VR Lab

Close the VR game, select a StrideXR profile, and choose **VR Lab → Open 3D test scene**. The 3D grid and controller markers let you try the profile's forward and reverse motion. The overlay sample uses the same renderer as Fallout.

- The right stick adjusts the selected setting. Press **A** to cycle between position, opacity, line count, and subtitle layout.
- Load your audiobook on the desktop first. In VR Lab, **Left Menu** or **Y** plays or pauses it. Press **X** to show or hide subtitles and **B** to save the settings and exit. **Close VR session** on the desktop exits without applying the preview edits.
- For reverse, hold each controller upside down beside its corresponding side of your head and waggle. Reverse requires both tracked controllers and a change of motion direction. Forward swing keeps using the profile's thumbrest setting. Turn **Reverse waggle** off in **Swing settings** if you want to disable the gesture for that profile.

## Dishonored 2

1. Open **StrideXR** and check **Enabled** if it is off. This registers the OpenXR layers for your Windows user.
2. Connect the Quest through Virtual Desktop. Set the Streamer’s OpenXR runtime to **VDXR**, then start your modded Dishonored 2 as usual. Restart the game after installing StrideXR or saving changes.
3. In gameplay, put both thumbs on the capacitive pads next to the sticks. Swing your arms forward and back. Lift either thumb to stop, or use the physical left stick to take over. The game controls movement direction and speed from its normal stick settings.

StrideXR uses the faster part of each arm swing to set your walking speed through the turnaround. It eases down when you settle into a slower swing and builds gradually through the top of the speed range. One tracked hand can sustain motion if the other drops out. When position samples are missing or jump, StrideXR can use valid OpenXR velocity. If both signals drop out, the profile's stop delay and fade apply. Releasing the thumbrest gate still stops forward movement immediately.

StrideXR starts with a minimum stick output of 0.38, just above the mod’s normal keyboard movement threshold. The mod may use keyboard-style walking even though StrideXR provides an analog signal. Sprint still uses the mod’s normal controls.

## Settings and other games

- **New profile** starts detection for the next unassigned game. Keep StrideXR open, then launch the game. When a game begins an OpenXR VR session, StrideXR assigns its executable path to the profile and uses the profile for that launch.
- In **Games**, click **Refresh** to load running and recently started executables. The list stays in place until the next refresh. Confirmed OpenXR VR sessions appear first. Select an entry and click **Assign selected EXE** to associate it with the current profile. You can also browse for an executable. StrideXR reads assignments at game startup.
- **Hide selected** remembers an executable name across profiles and app restarts. To clear out background apps before launching a game, click **Refresh**, then **Hide all listed**. Turn on **Show hidden** to see saved names again, then select one and click **Unhide selected** to restore it.
- **Set as default** chooses the profile shown on startup. Turn on **Use the default profile for unassigned OpenXR games** to use its settings for unassigned games. Assignments take precedence over the fallback. Use normal, non-administrator game launches so the OpenXR loader can discover the per-user registration.
- **Rename** updates the selected profile's name. **Delete** removes the selected profile and its executable assignments. Click a column header in **Games** to sort it alphabetically, or click it again to reverse the sort.
- **Swing settings** saves thumbrest activation, speed and stick limits, peak hold, slowdown smoothing, stop delay and fade, top-end acceleration, swing smoothing, and start confirmation for each profile. Select **Save profile**, then relaunch the game to apply edits.
- Use **Choose game folder** to save the folder containing a profile’s launcher or shortcuts. **Open game folder** opens it in File Explorer. When the profile has an assigned executable with a full path, its folder can also be opened directly.
- **Input monitor** shows the runtime, each thumbrest and pose, whether the game’s left-stick action was detected, and the output from the motion filter. Leave the game focused and look at the monitor through a desktop mirror or have someone check it. A dashboard or focus change may stop controller input.
- In **Diagnostics**, click **Start capture** before launching the game. Try arm swing, then click **Stop capture**. **Open capture folder** opens the local report with the OpenXR runtime and registrations, observed game processes, the VDXR log when available, and StrideXR's input trace when it loads.

StrideXR supports 32-bit and 64-bit Windows OpenXR games with a matching runtime, a supported Touch interaction profile, visible thumbrest touch and controller poses, and an OpenXR left-stick movement action. The layer recognizes the usual two-axis action and a separately bound left-stick Y axis. A title that uses only native OpenVR, the Oculus PC SDK, keyboard, or XInput needs a different input path. If a title has an OpenXR launch option, use that mode and enable smooth movement. In VDXR, use Touch controller emulation if thumbrest input is unavailable while emulating Index controllers.

## Calibrate in VR

1. Close your VR game, connect your headset, and select a profile. Open **VR Lab** and click **Start calibration**.
2. In the headset, press **A** to record each pace, then touch the selected thumbrests and swing at that pace. Press **B** to repeat. After the three recordings, swing to preview the new curve and press **A** to finish.
3. Select the destination profile and click **Apply to selected profile**. The keep-fast checkbox leaves the destination’s full-speed setting in place. The separate **Medium stick input** in **Swing settings** controls how much stick the measured medium swing sends.

The moving lane previews normalized forward stick input. The game applies its own stick dead zone and movement speed. The slow arm-speed point holds the minimum stick input between activation and the recorded gentle swing.

## Disable or remove

Uncheck **Enabled** in StrideXR and restart the game to unload the layers. Running `StrideXR.exe --uninstall` unregisters them and removes the Start menu shortcut. Settings and the installed version live under `%LOCALAPPDATA%\StrideXR`, which the app opens from **Open data folder**.

## Build and test

Use Visual Studio 2022 with the C++ desktop workload, Windows SDK, CMake, and the .NET Framework C# compiler on Windows. Run this from the repository root.

```powershell
.\Build.ps1
```

The script builds both layer architectures, the audio engine, the VR Lab/calibration app, and the simulated runtimes. It writes the packaged control panel to `build\StrideXR.exe`. The bundled x64 OpenXR loader is used by default. Both x64 and x86 loaders are included under `tools\openxr`, with their license notices in `third_party`. To use a loader you build yourself, pass `-OpenXRLoader <path-to-x64-openxr_loader.dll>`. The Khronos SDK revision is listed below.

After enabling the built layers, the packaged integration checks can be run with these commands.

```powershell
.\tests\Test.ps1 -Loader .\tools\openxr\x64\openxr_loader.dll
.\tests\Test.ps1 -Loader .\tools\openxr\x86\openxr_loader.dll -BuildDirectory .\build\Win32 -Disable64BitLayer
.\tests\TestCalibration.ps1
```

After enabling the built layers, run `tests/Test.ps1 -Loader <path-to-x64-openxr_loader.dll>` for the 64-bit integration suite. To test 32-bit, pass a Win32 loader and add `-BuildDirectory <build-directory>/Win32`. Add `-CheckActiveRuntime` to exercise action setup on the active VDXR runtime without opening a VR session. The simulations cover the app’s binding replacement path, input gates, motion, manual override, tracking loss, focus loss, and runtimes that reject the layer’s extra actions.

OpenXR headers are from Khronos OpenXR-SDK commit `24a1f2b92d212501a2b1b8d30ffd53abf55fcfa8`, under the MIT option in their dual license. See `third_party/OpenXR-MIT.txt`.
The bundled OpenXR loader uses the same SDK revision. See `third_party/OpenXR-loader-LICENSE.txt` and `third_party/JsonCpp-LICENSE.txt`. Run `tests/TestCalibration.ps1` to replay all three recordings against the simulated runtime.

MP3 decoding uses minimp3 revision `ea99364f61c14656440e8d77e9c233ccf3124633`. See `third_party/minimp3/LICENSE`. Audio output uses the Windows XAudio2 system library.

## Validation

The packaged Windows build was checked with real MP3 playback and automated cue timing, seeking, looping, pause/fade, and resume checks. Simulated OpenXR checks cover the 32-bit TTWVR overlay path, controller play/pause, locomotion, and the stereo VR Lab scene. These simulations do not establish physical controller behavior or in-headset readability for every runtime and game.

Audiobook overlays currently target `FalloutNV.exe` using D3D11 OpenXR. Compatibility with other games and runtimes depends on their input and graphics setup.
