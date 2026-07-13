Fallback 32-bit ARM libraries are NOT bundled in this package.

On muOS (and other PortMaster-capable firmware) the launcher uses the
device's own 32-bit libraries in /usr/lib32 first, so this folder can stay
empty and the game will run fine.

If your firmware is missing a library (check log.txt for "cannot open
shared object file"), drop the armhf .so files here — this folder is on the
launcher's LD_LIBRARY_PATH as a fallback. Typical candidates:
libSDL2-2.0.so.0, libstdc++.so.6, libgcc_s.so.1.
