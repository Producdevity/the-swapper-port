## Game Information

- **New Port for**: The Swapper
- **URL**: https://store.steampowered.com/app/231160/The_Swapper/

## Authorship & Testing

- [x] I wrote and understand this script/patch myself, or clearly marked which parts came from an AI assistant and reviewed them
- [x] I can explain every non-standard line in my launch script

## Submission Requirements

### CFW Tests

Ensure your game has been tested on all major CFWs:

- [x] ArkOS (R36S / RK3326 not supported)
- [ ] AmberELEC
- [ ] ROCKNIX
- [x] muOS
- [x] Knulli
- [ ] Crossmix (Optional)
- [x] Other: dArkOS (R36S / RK3326 not supported)

### Resolution Tests

Test all major resolutions:

- [ ] 480x320 (Optional)
- [x] 640x480
- [ ] 720x720 (RGB30) (Optional)
- [ ] Higher resolutions (e.g., 1280x720)

## File Structure

- Your port should have the following structure:
  - portname/
    - port.json
    - README.md
    - screenshot.png
    - cover.png
    - gameinfo.xml
    - Port Name.sh
    - portname/
      - <portfiles here>

## Script Conventions

The launch script follows the standard PortMaster Mono lifecycle (tasksetter, CFW GL configuration via `libgl_${CFW_NAME}.txt`, `pm_finish`). Non-standard elements:

- **First-launch patcher**: ASTC texture compression + normal-map downscaling to fit GPU memory budget on 1GB devices. Uses PortMaster's patcher UI (`utils/patcher.txt`).
- **ASTC runtime hook** (`libtexture_astc.so` via `LD_PRELOAD`): intercepts `glTexImage2D` at runtime to substitute pre-compressed ASTC textures to reduce VRAM usage.
- **FMOD compatibility layer** (`libfmodex.so`): translates FMOD audio calls to SDL_mixer, including streaming for large audio assets.
- **Steam shim** (`libsteam_api.so`): reports Steam as unavailable. Does not emulate Steam, bypass ownership, or decrypt tickets.
- **`MONO_MANAGED_WATCHER=1`**: required to prevent a Mono FileSystemWatcher infinite-recursion crash on Linux.
- **gl4es**: bundled in `gl4es.aarch64/` and loaded via `SDL_VIDEO_GL_DRIVER`. The game's engine requires desktop GL version strings that GLES-only devices don't provide.

## Additional Resources

For an in-depth guide on creating a pull request, refer to: [PortMaster Game Packaging Guide](https://portmaster.games/packaging.html#creating-a-pull-request)
