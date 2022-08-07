cmake -S ./ -B ./CMakeOut/Build/Debug -DCMAKE_INSTALL_PREFIX="./CMakeOut/Install/Debug"
cmake --build ./CMakeOut/Build/Debug/ --config Debug

cmake -S ./ -B ./CMakeOut/Build/Release -DCMAKE_INSTALL_PREFIX="./CMakeOut/Install/Release"
cmake --build ./CMakeOut/Build/Release/ --config Release
