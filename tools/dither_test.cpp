// dither-test: applies the generated plan table to a png, so image quality can be judged
// with no compositor running.
//
//   ./dither-test --palette orange-palette.png --in scene.png --out scene-dithered.png
//   ./dither-test --palette orange-palette.png --in scene.png --pixel-size 2 --zoom 4
//
// It runs the SAME logic the shader describes: snap to a pixelSize x pixelSize block,
// quantise to the top keyBits bits per channel, look up the plan, pick with Bayer, then
// paint the block. If this looks wrong, the shader will look wrong, and vice versa.

#include "dither_core.hpp"
#include "image_io.hpp"

#include <algorithm>
#include <format>
#include <iostream>
#include <string>
#include <vector>

using namespace Hyprdecor::Dither;

namespace
{
    std::vector<RGB> readStrip(std::string const &path, std::vector<uint8_t> &alpha)
    {
        const auto       IMG = ImageIO::load(path);
        std::vector<RGB> out;
        for (size_t i = 0; i < (size_t)IMG.width * IMG.height; i++)
        {
            out.push_back({IMG.rgba[i * 4], IMG.rgba[i * 4 + 1], IMG.rgba[i * 4 + 2]});
            alpha.push_back(IMG.rgba[i * 4 + 3]);
        }
        return out;
    }

    void usage()
    {
        std::cout <<
            "usage: dither-test --palette <strip.png> --in <image.png> [options]\n"
            "\n"
            "  --palette <path>    palette strip png\n"
            "  --in <path>         image to dither\n"
            "  --out <path>        output png (default <in>-dithered.png)\n"
            "  --key-bits <n>      bits per channel (default 5)\n"
            "  --psychovisual <f>  psychovisual weight (default 0.1)\n"
            "  --pixel-size <n>    block size, 1 = full resolution (default 1 for previews;\n"
            "                      the shader defaults to 2)\n"
            "  --zoom <n>          nearest-neighbour upscale of the OUTPUT, for inspection\n"
            "  --full-res          force pixel-size 1 even if the shader default is 2\n";
    }
}

int main(int argc, char **argv)
{
    std::string palette, in, out;
    int         keyBits = DEFAULT_KEY_BITS;
    double      psycho  = 0.1;
    int         pixelSize = 1;
    int         zoom      = 1;

    for (int i = 1; i < argc; i++)
    {
        const std::string A = argv[i];
        auto next = [&](char const *n) {
            if (i + 1 >= argc)
            {
                std::cerr << "dither-test: " << n << " needs a value\n";
                std::exit(2);
            }
            return std::string(argv[++i]);
        };

        if (A == "--help" || A == "-h")
        {
            usage();
            return 0;
        }
        else if (A == "--palette")
            palette = next("--palette");
        else if (A == "--in")
            in = next("--in");
        else if (A == "--out")
            out = next("--out");
        else if (A == "--key-bits")
            keyBits = std::stoi(next("--key-bits"));
        else if (A == "--psychovisual")
            psycho = std::stod(next("--psychovisual"));
        else if (A == "--pixel-size")
            pixelSize = std::stoi(next("--pixel-size"));
        else if (A == "--zoom")
            zoom = std::stoi(next("--zoom"));
        else if (A == "--full-res")
            pixelSize = 1;
        else
        {
            std::cerr << "dither-test: unknown argument " << A << "\n";
            return 2;
        }
    }

    if (palette.empty() || in.empty())
    {
        usage();
        return 2;
    }

    if (out.empty())
    {
        const auto DOT = in.find_last_of('.');
        out            = (DOT == std::string::npos ? in : in.substr(0, DOT)) + "-dithered.png";
    }

    pixelSize = std::max(1, pixelSize);
    zoom      = std::max(1, zoom);

    std::vector<uint8_t> alpha;
    const auto           PAL = dedupe(readStrip(palette, alpha), alpha);
    if (PAL.colours.size() < 2)
    {
        std::cerr << "dither-test: palette needs at least 2 unique opaque colours\n";
        return 1;
    }

    const auto LUT = buildLut(PAL, keyBits, psycho);

    ImageIO::Image img;
    try
    {
        img = ImageIO::load(in);
    }
    catch (std::exception const &e)
    {
        std::cerr << "dither-test: " << e.what() << "\n";
        return 1;
    }

    const uint32_t W = img.width, H = img.height;
    std::vector<uint8_t> result((size_t)W * H * 4);

    // Paint block by block, exactly as the shader samples one colour per block.
    for (uint32_t by = 0; by < H; by += (uint32_t)pixelSize)
    {
        for (uint32_t bx = 0; bx < W; bx += (uint32_t)pixelSize)
        {
            const uint32_t SX = std::min(bx + (uint32_t)pixelSize / 2, W - 1);
            const uint32_t SY = std::min(by + (uint32_t)pixelSize / 2, H - 1);
            const uint8_t *P  = &img.rgba[((size_t)SY * W + SX) * 4];
            const uint8_t  A  = P[3];

            RGB src{P[0], P[1], P[2]};
            if (A == 0)

            {
                // Leave transparency alone; the shader also passes alpha through.
                for (uint32_t y = by; y < std::min(by + (uint32_t)pixelSize, H); y++)
                    for (uint32_t x = bx; x < std::min(bx + (uint32_t)pixelSize, W); x++)
                    {
                        uint8_t *O = &result[((size_t)y * W + x) * 4];
                        O[0] = O[1] = O[2] = O[3] = 0;
                    }
                continue;
            }

            const auto PLAN = unpackPlan(LUT[keyFor(src, keyBits)]);

            // Bayer is keyed to the BLOCK, matching the shader's gl_FragCoord/PIXEL_SIZE.
            const uint8_t B      = BAYER4[(size_t)(((by / (uint32_t)pixelSize) & 3) * 4 + ((bx / (uint32_t)pixelSize) & 3))];
            const bool    SECOND = (int)B < PLAN.level;
            const RGB     OUT    = PAL.colours[SECOND ? PLAN.second : PLAN.first];

            for (uint32_t y = by; y < std::min(by + (uint32_t)pixelSize, H); y++)
                for (uint32_t x = bx; x < std::min(bx + (uint32_t)pixelSize, W); x++)
                {
                    uint8_t *O = &result[((size_t)y * W + x) * 4];
                    O[0]       = OUT[0];
                    O[1]       = OUT[1];
                    O[2]       = OUT[2];
                    O[3]       = A;
                }
        }
    }

    if (zoom > 1)
    {
        const uint32_t       ZW = W * (uint32_t)zoom, ZH = H * (uint32_t)zoom;
        std::vector<uint8_t> z((size_t)ZW * ZH * 4);
        for (uint32_t y = 0; y < ZH; y++)
            for (uint32_t x = 0; x < ZW; x++)
                for (int k = 0; k < 4; k++)
                    z[((size_t)y * ZW + x) * 4 + k] = result[((size_t)(y / zoom) * W + (x / zoom)) * 4 + k];
        ImageIO::save(out, ZW, ZH, z);
    }
    else
        ImageIO::save(out, W, H, result);

    std::cout << std::format("dither-test: {}x{} -> {} ({} palette colours, key {} bits, block {})\n", W, H, out, PAL.colours.size(), keyBits,
                             pixelSize);
    return 0;
}
