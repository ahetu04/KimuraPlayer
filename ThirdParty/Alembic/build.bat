SET CONFIG=Release
SET ZLIB_DIR=zlib-1.2.11
SET OPENEXR_DIR=openexr-2.5.2
SET ALEMBIC_DIR=alembic-1.7.16
SET GENERATOR="Visual Studio 16 2019"

:: zlib, both Debug and Release binaries are installed in the same folders
cmake -S ./%ZLIB_DIR% -B ./CMakeOut/Build/%ZLIB_DIR% -DCMAKE_INSTALL_PREFIX="./CMakeOut/Install/%ZLIB_DIR%"
cmake --build ./CMakeOut/Build/%ZLIB_DIR% --config Release
cmake --install ./CMakeOut/Build/%ZLIB_DIR% --config Release
cmake --build ./CMakeOut/Build/%ZLIB_DIR% --config Debug
cmake --install ./CMakeOut/Build/%ZLIB_DIR% --config Debug

:: OpenEXR (depends on zlib), both Debug and Release binaries are installed in the same folder
cmake -S ./%OPENEXR_DIR% -B ./CMakeout/Build/%OPENEXR_DIR% -DZLIB_LIBRARY=%~dp0CMakeOut/Install/%ZLIB_DIR%/lib/zlibstatic.lib -DZLIB_INCLUDE_DIR="./CMakeOut/Install/%ZLIB_DIR%/include" -DCMAKE_INSTALL_PREFIX="./CMakeOut/Install/%OPENEXR_DIR%" -DOPENEXR_BUILD_BOTH_STATIC_SHARED=TRUE -DILMBASE_BUILD_BOTH_STATIC_SHARED=TRUE
cmake --build ./CMakeOut/Build/%OPENEXR_DIR% --config Release
cmake --install ./CMakeOut/Build/%OPENEXR_DIR% --config Release
cmake --build ./CMakeOut/Build/%OPENEXR_DIR% --config Debug
cmake --install ./CMakeOut/Build/%OPENEXR_DIR% --config Debug

:: Alembic (depends on OpenEXR), Debug and Release binaries are installed is separate folders.
:: Build the release configuration first.
cmake -S ./%ALEMBIC_DIR% -B ./CMakeOut/Build/%ALEMBIC_DIR%/%CONFIG% -DCMAKE_INSTALL_PREFIX="./CMakeOut/Install/%ALEMBIC_DIR%/%CONFIG%" -DILMBASE_ROOT=%~dp0CMakeOut/Install/%OPENEXR_DIR% -DILMBASE_INCLUDE_DIR=%~dp0CMakeOut/Install/%OPENEXR_DIR%/include/OpenEXR -DALEMBIC_SHARED_LIBS=OFF -DUSE_TESTS=OFF -DUSE_BINARIES=OFF -DALEMBIC_ILMBASE_LINK_STATIC=ON -G %GENERATOR%
cmake --build ./CMakeOut/Build/%ALEMBIC_DIR%/%CONFIG%/ --config %CONFIG%
cmake --install ./CMakeOut/Build/%ALEMBIC_DIR%/%CONFIG%/ --config %CONFIG% --prefix "./CMakeOut/Install/%ALEMBIC_DIR%/%CONFIG%"

:: Then build the debug configuration.
SET CONFIG=Debug
cmake -S ./%ALEMBIC_DIR% -B ./CMakeOut/Build/%ALEMBIC_DIR%/%CONFIG% -DCMAKE_INSTALL_PREFIX="./CMakeOut/Install/%ALEMBIC_DIR%/%CONFIG%" -DILMBASE_ROOT=%~dp0CMakeOut/Install/%OPENEXR_DIR% -DILMBASE_INCLUDE_DIR=%~dp0CMakeOut/Install/%OPENEXR_DIR%/include/OpenEXR -DALEMBIC_SHARED_LIBS=OFF -DUSE_TESTS=OFF -DUSE_BINARIES=OFF -DALEMBIC_ILMBASE_LINK_STATIC=ON -G %GENERATOR%
cmake --build ./CMakeOut/Build/%ALEMBIC_DIR%/%CONFIG%/ --config %CONFIG%
cmake --install ./CMakeOut/Build/%ALEMBIC_DIR%/%CONFIG%/ --config %CONFIG% --prefix "./CMakeOut/Install/%ALEMBIC_DIR%/%CONFIG%"
