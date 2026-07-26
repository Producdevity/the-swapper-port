# The Swapper - Linux ARM Port

The Swapper ported to PortMaster compatible devices and CFW's. 
This is for the Windows [Steam release of The Swapper](https://store.steampowered.com/app/231160/The_Swapper/).

**This repository does not contain game data. Users must provide their own
legally owned copy of the game.**

<img width="4032" height="3024" alt="TheSwapper_Anbernic_RG40XX-H" src="https://github.com/user-attachments/assets/33d81d3d-9d48-46e3-81b9-ffc88783fbfe" />

## Game Data

Copy the contents of the Steam install folder into:

```text
ports/theswapper/gamedata/
```

The folder should contain `TheSwapper.exe`, `mainSettings.ini`, and the `data`
directory.

## Runtime

The game is a .NET Framework 4 assembly. The port runs it through PortMaster's
Mono runtime and supplies native Linux replacements for the Windows-only native
libraries used by the game. Some assets are downscaled and/or compressed to fit
in memory.

## Layout

- `src/`: native compatibility libraries.
- `packaging/`: PortMaster launcher, setup script, metadata, and seed profile.
- `scripts/`: build and deployment helpers.
- `build/`: generated output.
- `Dockerfile`: AArch64 build environment.

## Build

Build the native compatibility libraries:

```bash
make
```

Or build in Docker:

```bash
scripts/build-docker.sh
```

Build a PortMaster staging tree:

```bash
make package
```

The package template includes the PortMaster support libraries required by this
port. The build step adds the local compatibility shims into `libs.aarch64`.

## Deploy

After building, deploy the package over SSH:

```bash
scripts/deploy-muos.sh <ssh-host>
```

Deploy to a Knulli device:

```bash
scripts/deploy-knulli.sh <ssh-host>
```

Deploy to a dArkOS (UNSUPPORTED) device:

```bash
scripts/deploy-darkos.sh <ssh-host>
```

To test the same install path users will use, copy the zip into PortMaster's
autoinstall folder:

```bash
scripts/deploy-muos.sh --autoinstall <ssh-host>
scripts/deploy-knulli.sh --autoinstall <ssh-host>
scripts/deploy-darkos.sh --autoinstall <ssh-host>
```

Then open PortMaster on the device. PortMaster will install the zip and update
the frontend metadata.

The deploy scripts use the standard single-card PortMaster paths by default.
The Knulli and dArkOS helpers also accept explicit directories if you have 2 SD card setup:

```bash
scripts/deploy-knulli.sh --ports-dir /userdata/roms/ports <ssh-host>
scripts/deploy-knulli.sh --autoinstall-dir /userdata/system/.local/share/PortMaster/autoinstall <ssh-host>
scripts/deploy-darkos.sh --ports-dir /roms/ports <ssh-host>
scripts/deploy-darkos.sh --autoinstall-dir /roms/tools/PortMaster/autoinstall <ssh-host>
```

The host can also be provided with `SWAPPER_DEPLOY_HOST`.

For local testing, `SWAPPER_GAMEFILES_DIR` can point at a Steam install folder
to copy unmodified game files into `ports/theswapper/gamedata`.

```bash
SWAPPER_GAMEFILES_DIR=/path/to/The\ Swapper \
scripts/deploy-muos.sh <ssh-host>
```

Set `SWAPPER_RESET_SETUP=1` to force the first-launch setup step to run again on
the next launch. Use in combination with `SWAPPER_GAMEFILES_DIR` to avoid issues
where files already have been partially patched.

## First Launch

The launcher uses PortMaster's patcher UI for first-run setup. The setup step
validates the game files, installs the Mono DLL map, downscales selected texture
assets in the user's local `gamedata` copy, seeds a low-spec profile if no
profile exists yet, and marks setup complete. The low-spec profile just turns off
all improved graphic settings. It'll be ugly, but at least it'll be running above 12fps.
