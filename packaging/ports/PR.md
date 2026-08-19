## Game Information

- **New Port for**: The Swapper
- **URL**: https://store.steampowered.com/app/231160/The_Swapper/

## Authorship & Testing

- [x] I wrote and understand this script/patch myself, or clearly marked which parts came from an AI assistant and reviewed them
- [x] I can explain every non-standard line in my launch script

## Submission Requirements

### CFW Tests

Ensure your game has been tested on all major CFWs:

- [ ] ArkOS
- [ ] AmberELEC
- [x] ROCKNIX (RG40XX-H / H700, Panfrost)
- [x] muOS (RG35XX-H / H700)
- [x] Knulli (RG40XX-H / H700)
- [ ] Crossmix (Optional)
- [x] dArkOS (R36S / RK3326)

### Resolution Tests

Test all major resolutions:

- [ ] 320x240 (Optional)
- [x] 640x480
- [ ] 1024x768
- [ ] 1280x720
- [ ] 720x720

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

- **First-launch patcher**: uses `utils/patcher.txt` to build the ASTC texture cache without modifying the game assets.
- **Texture compression hook** (`libtexture_astc.so` via `LD_PRELOAD`): substitutes pre-compressed ASTC textures on supported drivers. Mali-G31 Panfrost uses runtime BC3 compression because its advertised native ASTC path crashes when sampled. On gl4es, alpha-heavy font and TGA detail textures stay uncompressed because its otherwise accepted ASTC upload renders transparent texels as opaque on the Mali blob.
- **FMOD compatibility layer** (`libfmodex.so`): translates FMOD audio calls to SDL_mixer, including streaming for large audio assets.
- **Steam shim** (`libsteam_api.so`): reports Steam as unavailable. Doesn't emulate, bypass ownership, or decrypt tickets.
- **`MONO_MANAGED_WATCHER=1`**: to prevent a Mono FileSystemWatcher infinite-recursion crash.

## Additional Resources

For an in-depth guide on creating a pull request, refer to: [PortMaster Game Packaging Guide](https://portmaster.games/packaging.html#creating-a-pull-request)
