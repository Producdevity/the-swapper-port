Native ARM libraries for this package.

Expected names after dllmap resolution:
- libfmodex.so
- libsteam_api.so
- libsdkencryptedappticket.so
- libwindows_stub.so
- kernel32 / libkernel32.so / Kernel32 aliases for Mono's Windows P/Invoke probes
- libSDL2_image.so / libSDL2_image.dll aliases for embedded SDL2# imports
- libSDL2_mixer.so / libSDL2_mixer.dll aliases for embedded SDL2# imports
- libSDL2_ttf.so / libSDL2_ttf.dll aliases for embedded SDL2# imports
- libSDL2_image-2.0.so.0 and its image codec dependencies
- libSDL2_mixer-2.0.so.0 and its audio codec dependencies
- libSDL2_ttf-2.0.so.0 and libfreetype.so.6

SDL2 itself should come from the firmware or PortMaster runtime environment.
SDL2_image, SDL2_mixer, and SDL2_ttf are staged here because the Steam build's
managed wrappers import those library names directly.
