"""Generate CShellModern.vcxproj: nolfsrc's modern CShell.dll with VS Build Tools.

Mirrors build/build.mk's cshell rule (sources from the .dsp, src/modern/cshell,
NOLFSRC_MODERN, final defines) and links the lithshared libraries the
Modernizer CShell build already uses.
"""
import glob, os, re

HERE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "src", "game", "ClientShellDLL")
M = os.environ.get("MODERNIZER_LITHSHARED", r"C:\nolf1-modernizer\LT2\lithshared")
os.chdir(HERE)

t = open("ClientShellDLL.dsp", encoding="latin1").read().replace("\r", "")
srcs = []
for b in re.split(r"\n# Begin Source File\n", t)[1:]:
    s = re.search(r"SOURCE=(\S+)", b).group(1).strip('"')
    if s.lower().endswith(".cpp") and "stdafx.cpp" not in s.lower():
        srcs.append(s)
mod = sorted(glob.glob(r"..\..\modern\cshell\*.cpp"))
extra = sorted(glob.glob(r"*.cpp.extra"))  # none yet

inc = ";".join([r"..\shared", r"..\..\LT2\sdk\inc", M + r"\butemgr", M + r"\cryptmgr",
                M + r"\mfcstub", M + r"\lith", r"..\..\LT2\lithshared\incs", ".",
                r"..\..\modern", r"..\ClientRes"])
libdirs = ";".join([M + r"\built\Release\ButeMgr", M + r"\built\Release\CryptMgr",
                    M + r"\built\Release\MFCStub", M + r"\libs\Release"])
items = "\n".join(f'    <ClCompile Include="{s}" />' for s in srcs)
# nolfsrc's own GameSpy replacement (has GameSpyClient_SetMasterServer)
items += ("\n    <ClCompile Include=\"..\\..\\lithshared\\gamespy\\GameSpyClientMgr.cpp\">"
          "<PrecompiledHeader>NotUsing</PrecompiledHeader>"
          "<AdditionalIncludeDirectories>..\\..\\lithshared\\gamespy;" + M + "\\mfcstub;"
          "%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>"
          "<ObjectFileName>$(IntDir)gamespy\\</ObjectFileName></ClCompile>")
items += "\n" + "\n".join(
    f'    <ClCompile Include="{m}"><PrecompiledHeader>NotUsing</PrecompiledHeader>'
    f'<ObjectFileName>$(IntDir)modern\\</ObjectFileName></ClCompile>' for m in mod)

proj = f"""<?xml version="1.0" encoding="utf-8"?>
<Project DefaultTargets="Build" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <ItemGroup Label="ProjectConfigurations">
    <ProjectConfiguration Include="Release|Win32"><Configuration>Release</Configuration><Platform>Win32</Platform></ProjectConfiguration>
  </ItemGroup>
  <PropertyGroup Label="Globals"><ProjectGuid>{{6E1E5C55-6D1F-4C2B-9A41-1B6E2C3D4F51}}</ProjectGuid><Keyword>Win32Proj</Keyword><WindowsTargetPlatformVersion>10.0</WindowsTargetPlatformVersion></PropertyGroup>
  <Import Project="$(VCTargetsPath)\\Microsoft.Cpp.Default.props" />
  <PropertyGroup Label="Configuration">
    <ConfigurationType>DynamicLibrary</ConfigurationType><PlatformToolset>v145</PlatformToolset><CharacterSet>MultiByte</CharacterSet><UseOfMfc>false</UseOfMfc>
  </PropertyGroup>
  <Import Project="$(VCTargetsPath)\\Microsoft.Cpp.props" />
  <PropertyGroup><OutDir>.\\Modern_Release\\</OutDir><IntDir>.\\Modern_Release\\</IntDir><TargetName>CShell</TargetName><LinkIncremental>false</LinkIncremental></PropertyGroup>
  <ItemDefinitionGroup>
    <ClCompile>
      <RuntimeLibrary>MultiThreaded</RuntimeLibrary><Optimization>MaxSpeed</Optimization><StringPooling>true</StringPooling><FunctionLevelLinking>true</FunctionLevelLinking>
      <WarningLevel>Level1</WarningLevel><SDLCheck>false</SDLCheck><ConformanceMode>false</ConformanceMode>
      <AdditionalIncludeDirectories>{inc};%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>
      <PreprocessorDefinitions>_REZFILE;_FINAL;NDEBUG;_CLIENTBUILD;WIN32;_WINDOWS;_NOLFBUILD;NO_PRAGMA_LIBS;NOLFSRC_MODERN;_CRT_SECURE_NO_WARNINGS;_CRT_NONSTDC_NO_DEPRECATE;%(PreprocessorDefinitions)</PreprocessorDefinitions>
      <PrecompiledHeader>Use</PrecompiledHeader><PrecompiledHeaderFile>stdafx.h</PrecompiledHeaderFile>
      <LanguageStandard>stdcpp14</LanguageStandard><ForceConformanceInForLoopScope>false</ForceConformanceInForLoopScope>
      <MultiProcessorCompilation>true</MultiProcessorCompilation><AdditionalOptions>/Zc:strictStrings- /permissive %(AdditionalOptions)</AdditionalOptions>
    </ClCompile>
    <Link>
      <SubSystem>Windows</SubSystem><ImageHasSafeExceptionHandlers>false</ImageHasSafeExceptionHandlers>
      <IgnoreSpecificDefaultLibraries>LIBCMTD;\\proj\\libs\\release\\StdLith.lib</IgnoreSpecificDefaultLibraries>
      <AdditionalLibraryDirectories>{libdirs};%(AdditionalLibraryDirectories)</AdditionalLibraryDirectories>
      <AdditionalDependencies>ButeMgr.lib;cryptmgr.lib;MFCStub.lib;StdLith.lib;RegMgr.lib;ltguimgr_LT2.lib;LithFontMgr_LT2.lib;winmm.lib;odbc32.lib;odbccp32.lib;%(AdditionalDependencies)</AdditionalDependencies>
      <GenerateMapFile>true</GenerateMapFile>
    </Link>
  </ItemDefinitionGroup>
  <ItemGroup>
    <ClCompile Include="..\\Shared\\Stdafx.cpp"><PrecompiledHeader>Create</PrecompiledHeader></ClCompile>
{items}
  </ItemGroup>
  <Import Project="$(VCTargetsPath)\\Microsoft.Cpp.targets" />
</Project>
"""
open("CShellModern.vcxproj", "w").write(proj)
print(len(srcs), "game +", len(mod), "modern sources")
