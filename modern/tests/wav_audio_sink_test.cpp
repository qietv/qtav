// SPDX-License-Identifier: LGPL-2.1-or-later

#include <qtav/wav_audio_sink.h>

#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

std::uint16_t read16(
    const std::vector<std::uint8_t>& data,
    std::size_t offset)
{
    return static_cast<std::uint16_t>(data[offset])
        | static_cast<std::uint16_t>(data[offset + 1] << 8U);
}

std::uint32_t read32(
    const std::vector<std::uint8_t>& data,
    std::size_t offset)
{
    std::uint32_t result = 0;
    for (std::size_t byte = 0; byte < 4; ++byte) {
        result |= static_cast<std::uint32_t>(data[offset + byte])
            << static_cast<unsigned>(byte * 8U);
    }
    return result;
}

std::vector<std::uint8_t> readFile(const std::string& path)
{
    std::ifstream input(path, std::ios::binary);
    assert(input);
    return {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>(),
    };
}

} // namespace

int main(int argc, char** argv)
{
    assert(argc == 2);
    const std::string path = argv[1];
    std::remove(path.c_str());

    const qtav::AudioFormat format {
        8'000,
        2,
        qtav::SampleFormat::S16,
        "stereo",
    };
    const std::array<std::int16_t, 4> samples {
        static_cast<std::int16_t>(0x1122),
        static_cast<std::int16_t>(0x3344),
        static_cast<std::int16_t>(0x5566),
        static_cast<std::int16_t>(0x7788),
    };
    std::array<std::uint8_t, sizeof(samples)> storage {};
    std::memcpy(storage.data(), samples.data(), storage.size());

    qtav::WavAudioSink sink({
        path,
        0,
        0,
        qtav::SampleFormat::S16,
        {},
    });
    const auto opened = sink.open(format);
    assert(opened.success);
    assert(opened.deviceFormat.sampleRate == format.sampleRate);
    assert(opened.deviceFormat.channels == format.channels);
    assert(opened.deviceFormat.channelLayout == format.channelLayout);
    assert(sink.outputFormat().sampleFormat == qtav::SampleFormat::S16);
    assert(!sink.clock().valid);

    const qtav::AudioBufferView buffer {
        format,
        2,
        { storage.data() },
        { static_cast<int>(storage.size()) },
        0,
        1,
    };
    sink.pause(true);
    assert(!sink.write(buffer));
    sink.pause(false);
    assert(sink.write(buffer));
    sink.flush();
    assert(sink.drain());
    assert(sink.bytesWritten() == storage.size());
    sink.close();

    const auto file = readFile(path);
    assert(file.size() == 44 + storage.size());
    assert(std::memcmp(file.data(), "RIFF", 4) == 0);
    assert(read32(file, 4) == 36 + storage.size());
    assert(std::memcmp(file.data() + 8, "WAVEfmt ", 8) == 0);
    assert(read32(file, 16) == 16);
    assert(read16(file, 20) == 1);
    assert(read16(file, 22) == 2);
    assert(read32(file, 24) == 8'000);
    assert(read32(file, 28) == 32'000);
    assert(read16(file, 32) == 4);
    assert(read16(file, 34) == 16);
    assert(std::memcmp(file.data() + 36, "data", 4) == 0);
    assert(read32(file, 40) == storage.size());
    assert(file[44] == 0x22);
    assert(file[45] == 0x11);
    assert(file[46] == 0x44);
    assert(file[47] == 0x33);

    qtav::WavAudioSink planar({
        path,
        0,
        0,
        qtav::SampleFormat::S16Planar,
        {},
    });
    assert(!planar.open(format).success);

    std::remove(path.c_str());
    return 0;
}
