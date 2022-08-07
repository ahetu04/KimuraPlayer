:: Kimura Converter

cmake -S ./ -B ./CMakeOut/Build/Debug -DCMAKE_INSTALL_PREFIX="./CMakeOut/Install/Debug" -DKIMURACONVERTER_LINK_DEBUG_LIBS=ON
cmake --build ./CMakeOut/Build/Debug/ --config Debug
cmake --install ./CMakeOut/Build/Debug/ --config Debug

cmake -S ./ -B ./CMakeOut/Build/Release -DCMAKE_INSTALL_PREFIX="./CMakeOut/Install/Release"
cmake --build ./CMakeOut/Build/Release/ --config Release
cmake --install ./CMakeOut/Build/Release/ --config Release


