
# Kimura Player

**August 6th, 2022**

The Kimura Player is a simple set of libraries for optimizing and delivering alembic animations in real-time applications. The project's goal is to tie in complex, animated scenes created in digital content creation tools such as Houdini, Cinema4D, Blender, etc, to real-time rendering engines like Unreal, Unity, etc, in the most optimal way. 

# Solution and projects

This repository contains the project files for compiling Kimura's libraries (KimuraConverter and Player) and executables (abcToKimura and TestPlayer). The libraries contain the bulk of the code, and the executables are simply command-line utilities wrapping around the libraries. 

> Important: Both Visual Studio and CMake solutions require that the `Alembic` libraries be built prior to building the KimuraConverter project. See below for more info on building the dependencies. 

## Visual Studio
The simplest way to build the whole thing is through Visual Studio. The solution was recently upgrated to Visual Studio 2022 but it can be easily modified to work back again with Visual Studio 2019. 

## CMake
While the solution is better suited to Visual Studio and Windows, CMake files are provided and can be used to build the libraries on Linux as well. 

> Disclaimer: I try to keep the CMake setup as modern and simple as possible but I am not a CMake person, far from it. I have tried emulating the Visual Studio solution through what I believe is modern CMake, but this might just be another failed attempt at modern CMake and I wouldn't know about it. But please, feel free suggest improvements. 

The projects can also be built individually using CMake. Each project directory contains script files that will build both Debug and Release variants using CMake (in the simplest manner possible). 

Building the 'Debug' variant of AbcToKimura will require that the CMake option 'KIMURACONVERTER_LINK_DEBUG_LIBS' be set to 'ON' in order to link against the proper Alembic libraries. This is necessary for linking the 'Debug' configuration with MSVC.

> Note: For now, support for converting image sequences is only enabled on the Windows platform, through the Visual Studio solution. 

# Dependencies

* ``Alembic``
The KimuraConverter project has dependencies to Alembic-1.7.16, openexr-2.5.2 and zlib-1.2.11. They are included in the ThirdParty/Alembic directory and must be built before the KimuraConverter project. Simple scripts (build.bat and build.sh) were provided to automate the process. Building those dependencies require CMake and Python. 

* ``texconv``
The project, also located under the ThirdParty directory, is a modified version of DirectXTex's texconv command-line texture utility. It is used to convert image sequences and include them alongside alembic meshes in the Kimura files. 

> Note: For now, support for converting image sequences is only enabled on Windows through the Visual Studio solution. 

# Unreal Engine

A full integration of the Kimura Player for Unreal Engine can be found in a separate repository: [Kimura Player Unreal Plugin](https://github.com/ahetu04/KimuraPlayer-Unreal)

# Directory Layout

* ``KimuraConverter/``

  The entire conversion suite is wrapped in this single library and can be linked into other projects. 

* ``AbcToKimura/``

  A very simple executable wrapping the KimuraConverter library. This produces the executable/binary for converting alembic files to Kimura from a command-line console. 

* ``Player/``

  The player class used for reading a Kimura file and exposing its frames to a user. 

* ``TestPlayer/``

  A simple executable wrapping around the Player library and which reads frames from a specified .k file. 

# Documentation

See [Kimura Player Unreal Plugin](https://github.com/ahetu04/KimuraPlayer-Unreal)'s repository for more documentation on the Kimura Player libraries. 

# Notices

All content and source code for this package are subject to the terms of the [MIT License](LICENSE.md).

The KimuraConverter library and executable (AbcToKimura) are subject to the following licenses:
- [Alembic 1.7.16 LICENSE](ThirdParty/Alembic/alembic-1.7.16/LICENSE.txt)
- [OpenEXR 2.5.2 LICENSE](ThirdParty/Alembic/openexr-2.5.2/LICENSE.md)
- [zlib 1.2.11 README](ThirdParty/Alembic/zlib-1.2.11/README)
- [DirectXTex LICENSE](ThirdParty/DirectXTex/LICENSE)

# Credits
The Kimura Player libraries and Kimura Player plugin for Unreal were created by Alexandre Hetu.