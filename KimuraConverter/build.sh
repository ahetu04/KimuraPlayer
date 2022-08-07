cmake -S ./ -B ./CMakeOut/Build/Debug -DCMAKE_INSTALL_PREFIX="./CMakeOut/Install/Debug"
cmake --build ./CMakeOut/Build/Debug/ --config Debug
cmake --install ./CMakeOut/Build/Debug/ --config Debug
