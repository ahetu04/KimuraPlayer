#pragma once
#include <vector>

namespace Kimura
{
    struct Mipmap
    {
        int width;
        int height;
        int rowPitch;
        int slicePitch;
        std::vector<unsigned char> buffer;
    };

}

int texconv(_In_ int argc, _In_z_count_(argc) wchar_t* argv[], std::vector<Kimura::Mipmap>& OutMipmaps);
