# Building the modern CShell.dll with Visual Studio Build Tools

An unofficial way to build the modern build's `CShell.dll` on Windows without
Docker, Wine or the VC6 toolchain. The result is not byte-identical to the
project's build and is not what the releases ship; it is for trying game
code changes quickly.

What it needs:

* Visual Studio Build Tools with the v145 toolset (x86).
* The lithshared libraries from a built
  [NOLF Modernizer](https://github.com/haekb/nolf1-modernizer) tree
  (`LT2/lithshared`: `built/Release/{ButeMgr,CryptMgr,MFCStub}` built with
  that repository's projects, and its prebuilt `libs/Release`). Point
  `MODERNIZER_LITHSHARED` at that `LT2/lithshared` directory.

```bat
set MODERNIZER_LITHSHARED=C:\nolf1-modernizer\LT2\lithshared
python contrib\msvc\gen_vcxproj.py
msbuild src\game\ClientShellDLL\CShellModern.vcxproj -p:Configuration=Release -p:Platform=Win32 -m
```

The DLL lands in `src/game/ClientShellDLL/Modern_Release/CShell.dll`; copy it
over `custom/cshell.dll` in a modern game directory (keep the original).

The project compiles the `.dsp`'s sources, `src/modern/cshell/*.cpp` and
`src/lithshared/gamespy/GameSpyClientMgr.cpp` with `NOLFSRC_MODERN` and the
final-release defines, as `build/build.mk` does. Modern MSVC needed five small
source changes on this branch (default arguments on function pointers,
`std::` on `fstream`, a `const char*` cast).
