declare CONFIG=Release
declare ZLIB_DIR=zlib-1.2.11
declare OPENEXR_DIR=openexr-2.5.2
declare ALEMBIC_DIR=alembic-1.7.16

# zlib, both Debug and Release binaries are installed in the same folders
cmake -S ./$ZLIB_DIR -B ./CMakeOut/Build/$ZLIB_DIR -DCMAKE_INSTALL_PREFIX="./CMakeOut/Install/$ZLIB_DIR"
cmake --build ./CMakeOut/Build/$ZLIB_DIR --config Release
cmake --install ./CMakeOut/Build/$ZLIB_DIR --config Release
cmake --build ./CMakeOut/Build/$ZLIB_DIR --config Debug
cmake --install ./CMakeOut/Build/$ZLIB_DIR --config Debug

# OpenEXR (depends on zlib), both Debug and Release binaries are installed in the same folder
cmake -S ./$OPENEXR_DIR -B ./CMakeOut/Build/$OPENEXR_DIR -DZLIB_LIBRARY=$PWD/CMakeOut/Install/$ZLIB_DIR/lib/libz.so -DZLIB_INCLUDE_DIR="./CMakeOut/Install/$ZLIB_DIR/include" -DCMAKE_INSTALL_PREFIX="./CMakeOut/Install/$OPENEXR_DIR" -DOPENEXR_BUILD_BOTH_STATIC_SHARED=TRUE -DILMBASE_BUILD_BOTH_STATIC_SHARED=TRUE
cmake --build ./CMakeOut/Build/$OPENEXR_DIR --config Release
cmake --install ./CMakeOut/Build/$OPENEXR_DIR --config Release
cmake --build ./CMakeOut/Build/$OPENEXR_DIR --config Debug
cmake --install ./CMakeOut/Build/$OPENEXR_DIR --config Debug

cmake -S ./$ALEMBIC_DIR -B ./CMakeOut/Build/$ALEMBIC_DIR/$CONFIG -DCMAKE_INSTALL_PREFIX="./CMakeOut/Install/$ALEMBIC_DIR/$CONFIG" -DILMBASE_ROOT=$PWD/CMakeOut/Install/$OPENEXR_DIR -DILMBASE_INCLUDE_DIR=$PWD/CMakeOut/Install/$OPENEXR_DIR/include/OpenEXR -DALEMBIC_SHARED_LIBS=OFF -DUSE_TESTS=OFF -DUSE_BINARIES=OFF -DALEMBIC_ILMBASE_LINK_STATIC=ON
cmake --build ./CMakeOut/Build/$ALEMBIC_DIR/$CONFIG/ --config $CONFIG
cmake --install ./CMakeOut/Build/$ALEMBIC_DIR/$CONFIG/ --config $CONFIG --prefix "./CMakeOut/Install/$ALEMBIC_DIR/$CONFIG"

CONFIG=Debug
cmake -S ./$ALEMBIC_DIR -B ./CMakeOut/Build/$ALEMBIC_DIR/$CONFIG -DCMAKE_INSTALL_PREFIX="./CMakeOut/Install/$ALEMBIC_DIR/$CONFIG" -DILMBASE_ROOT=$PWD/CMakeOut/Install/$OPENEXR_DIR -DILMBASE_INCLUDE_DIR=$PWD/CMakeOut/Install/$OPENEXR_DIR/include/OpenEXR -DALEMBIC_SHARED_LIBS=OFF -DUSE_TESTS=OFF -DUSE_BINARIES=OFF -DALEMBIC_ILMBASE_LINK_STATIC=ON
cmake --build ./CMakeOut/Build/$ALEMBIC_DIR/$CONFIG/ --config $CONFIG
cmake --install ./CMakeOut/Build/$ALEMBIC_DIR/$CONFIG/ --config $CONFIG --prefix "./CMakeOut/Install/$ALEMBIC_DIR/$CONFIG"
