#pragma once

// Image I/O for the palette tools, backed by ImageMagick's `magick` binary.
//
// This deliberately replaces a hand-written PNG decoder. That decoder needed its own
// inflate implementation and kept failing on real-world streams (ImageMagick itself
// writes zlib streams with a preset dictionary), and the tool is a build-time helper,
// not something that ships. Shelling out to a tool every desktop already has is far
// less code to get wrong, and it reads indexed, 16-bit and colour-managed PNGs for free.
//
// The SHADER and the PLUGIN never use this; only the offline generators do.

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace Hyprdecor::Dither::ImageIO
{
    struct RGB
    {
        uint8_t r = 0, g = 0, b = 0;
    };

    struct Image
    {
        uint32_t             width  = 0;
        uint32_t             height = 0;
        std::vector<uint8_t> rgba; // width * height * 4

        const uint8_t *at(uint32_t x, uint32_t y) const
        {
            return &rgba[((size_t)y * width + x) * 4];
        }
    };

    // Quotes a path for /bin/sh so spaces and quotes cannot break the command.
    inline std::string shellQuote(std::string const &s)
    {
        std::string out = "'";
        for (char c : s)
        {
            if (c == '\'')
                out += "'\\''";
            else
                out += c;
        }
        out += "'";
        return out;
    }

    // Runs a command and captures stdout as bytes. Throws with the command on failure,
    // so a missing `magick` is an obvious error rather than an empty image.
    inline std::vector<uint8_t> run(std::string const &cmd)
    {
        FILE *pipe = popen(cmd.c_str(), "r");
        if (!pipe)
            throw std::runtime_error("image_io: cannot run: " + cmd);

        std::vector<uint8_t> out;
        uint8_t              buf[65536];
        size_t               n;
        while ((n = fread(buf, 1, sizeof(buf), pipe)) > 0)
            out.insert(out.end(), buf, buf + n);

        const int STATUS = pclose(pipe);
        if (STATUS != 0)
            throw std::runtime_error("image_io: command failed: " + cmd);

        return out;
    }

    // Reads any image magick understands as 8-bit RGBA.
    inline Image load(std::string const &path)
    {
        Image img;

        // Dimensions first, in a form that is trivial to parse and cannot be confused
        // by locale-dependent number formatting.
        const auto DIMS = run(std::format("magick {} -depth 8 -format '%w %h' info:", shellQuote(path)));
        const std::string DIMSSTR(DIMS.begin(), DIMS.end());

        if (sscanf(DIMSSTR.c_str(), "%u %u", &img.width, &img.height) != 2 || img.width == 0 || img.height == 0)
            throw std::runtime_error("image_io: cannot read dimensions of " + path);

        img.rgba = run(std::format("magick {} -depth 8 rgba:-", shellQuote(path)));

        const size_t EXPECT = (size_t)img.width * img.height * 4;
        if (img.rgba.size() != EXPECT)
            throw std::runtime_error(std::format("image_io: {} returned {} bytes, expected {} ({}x{})", path, img.rgba.size(), EXPECT, img.width,
                                                 img.height));

        return img;
    }

    // Writes raw RGBA to a PNG through magick.
    inline void save(std::string const &path, uint32_t w, uint32_t h, std::vector<uint8_t> const &rgba)
    {
        const std::string TMP = path + ".rgba";

        {
            std::ofstream f(TMP, std::ios::binary);
            if (!f)
                throw std::runtime_error("image_io: cannot write " + TMP);
            f.write((char const *)rgba.data(), (std::streamsize)rgba.size());
        }

        try
        {
            run(std::format("magick -size {}x{} -depth 8 rgba:{} {}", w, h, shellQuote(TMP), shellQuote(path)));
        }
        catch (...)
        {
            std::remove(TMP.c_str());
            throw;
        }

        std::remove(TMP.c_str());
    }
}
