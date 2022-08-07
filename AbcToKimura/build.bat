:: AbcToKimura

cmake -S ./ -B ./CMakeOut/Build/Debug -DCMAKE_INSTALL_PREFIX="./CMakeOut/Install/Debug" -DKIMURACONVERTER_LINK_DEBUG_LIBS=ON
cmake --build ./CMakeOut/Build/Debug/ --config Debug

cmake -S ./ -B ./CMakeOut/Build/Release -DCMAKE_INSTALL_PREFIX="./CMakeOut/Install/Release" -DKIMURACONVERTER_LINK_DEBUG_LIBS=OFF
cmake --build ./CMakeOut/Build/Release/ --config Release
