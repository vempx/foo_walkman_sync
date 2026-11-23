#include "sync.h"
#include <foobar2000/SDK/dsp_manager.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>

namespace wm {
// The settings database stores Unicode text. Keep opaque DSP presets byte-exact.
S serializeChain(const dsp_chain_config &chain) {
    if (!chain.get_count())
        return {};
    auto blob = chain.to_blob();
    const auto *p = static_cast<const unsigned char *>(blob->data());
    static constexpr char digits[] = "0123456789abcdef";
    S result;
    result.reserve(2 * blob->size());
    for (size_t i = 0; i < blob->size(); ++i) {
        result += digits[p[i] >> 4];
        result += digits[p[i] & 15];
    }
    return result;
}
void restoreChain(dsp_chain_config &chain, const S &text) {
    if (text.empty())
        return;
    auto digit = [](char c) {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        throw std::runtime_error(tr("badDspConfig"));
    };
    if (text.size() % 2)
        throw std::runtime_error(tr("badDspConfig"));
    std::vector<unsigned char> bytes;
    bytes.reserve(text.size() / 2);
    for (size_t i = 0; i < text.size(); i += 2)
        bytes.push_back((unsigned char)((digit(text[i]) << 4) | digit(text[i + 1])));
    chain.from_blob(bytes.data(), bytes.size());
}
std::vector<S> resamplers() {
    std::vector<S> names;
    for (auto entry : dsp_entry::enumerate()) {
        resampler_entry::ptr res;
        if (entry->cast(res))
            names.emplace_back(entry->get_name().c_str());
    }
    return names;
}
unsigned targetRate(const Device &d, unsigned source) {
    if (!d.usesFamilyResampler())
        return source;
    const bool family44 = source % 11025 == 0;
    const unsigned cap =
        d.format == "aac" ? (family44 ? d.appleRate44 : d.appleRate48) : (family44 ? d.rate44 : d.rate48);
    return std::min(source, cap);
}
static void addResampler(dsp_chain_config_impl &chain, const S &name, unsigned from, unsigned to) {
    if (from == to)
        return;
    for (auto entry : dsp_entry::enumerate()) {
        resampler_entry::ptr res;
        if (entry->get_name() != name.c_str() || !entry->cast(res))
            continue;
        dsp_preset_impl preset;
        if (res->is_conversion_supported(from, to) && res->create_preset(preset, to, 1.0f)) {
            chain.add_item(preset);
            return;
        }
    }
    throw std::runtime_error(message("resamplerUnavailable", {name}));
}
class Wave {
    std::ofstream out;
    uint64_t size = 0;
    WAVEFORMATEXTENSIBLE format{};
    bool pcm;
    template <typename T> void field(const T &v) { out.write((const char *)&v, sizeof(v)); }

  public:
    Wave(const fs::path &path, unsigned rate, unsigned channels, unsigned mask, unsigned bits, bool fixed)
        : pcm(fixed) {
        out.exceptions(std::ios::failbit | std::ios::badbit);
        out.open(path, std::ios::binary);
        auto &w = format.Format;
        w.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        w.nChannels = (WORD)channels;
        w.nSamplesPerSec = rate;
        w.wBitsPerSample = (WORD)((bits + 7) / 8 * 8);
        w.nBlockAlign = w.nChannels * w.wBitsPerSample / 8;
        w.nAvgBytesPerSec = rate * w.nBlockAlign;
        w.cbSize = 22;
        format.Samples.wValidBitsPerSample = (WORD)bits;
        format.dwChannelMask = mask;
        format.SubFormat = pcm ? KSDATAFORMAT_SUBTYPE_PCM : KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        out.write("RIFF", 4);
        field(uint32_t(0));
        out.write("WAVEfmt ", 8);
        field(uint32_t(sizeof(format)));
        field(format);
        out.write("data", 4);
        field(uint32_t(0));
    }
    void write(const audio_chunk &chunk) {
        auto &w = format.Format;
        if (chunk.get_sample_rate() != w.nSamplesPerSec || chunk.get_channels() != w.nChannels ||
            chunk.get_channel_config() != format.dwChannelMask)
            throw std::runtime_error(tr("dspOutputMismatch"));
        auto count = chunk.get_sample_count() * w.nChannels;
        mem_block_container_impl buf;
        if (pcm) {
            buf.set_size(count * w.wBitsPerSample / 8);
            if (!audio_chunk::g_toFixedPoint(chunk.get_data(), buf.get_ptr(), count, w.wBitsPerSample,
                                             format.Samples.wValidBitsPerSample))
                throw std::runtime_error(tr("restoreDepth"));
        } else if (!chunk.to_raw_data(buf, 32))
            throw std::runtime_error(tr("pcmFailed"));
        size += buf.get_size();
        if (size > UINT32_MAX - 80)
            throw std::runtime_error(tr("wavTooLarge"));
        out.write((const char *)buf.get_ptr(), buf.get_size());
    }
    void finish() {
        auto end = (uint64_t)out.tellp();
        if (size % 2) {
            out.put(0);
            ++end;
        }
        out.seekp(4);
        field(uint32_t(end - 8));
        out.seekp(24 + sizeof(format));
        field(uint32_t(size));
        out.close();
    }
};
S encode(const Song &song, const Device &device, const fs::path &dest, abort_callback &abort) {
    input_decoder::ptr decoder;
    input_entry::g_open_for_decoding(decoder, {}, song.handle->get_path(), abort);
    auto sub = song.handle->get_subsong_index();
    file_info_impl info;
    decoder->get_info(sub, info, abort);
    decoder->initialize(sub, input_flag_simpledecode, abort);
    audio_chunk_impl chunk;
    if (!decoder->run(chunk, abort))
        throw std::runtime_error(tr("noAudio"));
    bool extract = song.mode == Song::extract;
    auto target = extract ? chunk.get_sample_rate() : targetRate(device, chunk.get_sample_rate());
    int precision = extract ? 0 : device.output().bits;
    int bits = precision ? std::abs(precision) : (int)info.info_get_int("bitspersample");
    if (!extract && !precision && (bits < 8 || target != chunk.get_sample_rate() || !device.chain.empty()))
        bits = std::max(bits, 24);
    if (bits < 8 || bits > 32)
        throw std::runtime_error(tr("subsongDepth"));
    auto pcm = dest;
    pcm += L".wav";
    dsp_chain_config_impl chain;
    if (!extract) {
        restoreChain(chain, device.chain);
        addResampler(chain, device.resampler, chunk.get_sample_rate(), target);
    }
    std::vector<dsp::ptr> processors;
    for (size_t i = 0; i < chain.get_count(); ++i) {
        dsp::ptr dsp;
        if (!dsp_entry::g_instantiate(dsp, chain.get_item(i)))
            throw std::runtime_error(tr("dspUnavailable"));
        processors.push_back(dsp);
    }
    std::unique_ptr<Wave> wave;
    auto process = [&](bool end) {
        dsp_chunk_list_impl list;
        if (!end)
            list.add_chunk(&chunk);
        for (auto &dsp : processors)
            dsp->run_abortable(&list, song.handle, end ? dsp::END_OF_TRACK | dsp::FLUSH : 0, abort);
        for (size_t i = 0; i < list.size(); ++i) {
            const auto &c = list[i];
            if (!wave)
                wave = std::make_unique<Wave>(
                    pcm, extract || device.usesFamilyResampler() ? target : c.get_sample_rate(),
                    c.get_channels(), c.get_channel_config(), bits, precision >= 0);
            wave->write(c);
        }
    };
    do {
        abort.check();
        process(false);
    } while (decoder->run(chunk, abort));
    process(true);
    if (!wave)
        throw std::runtime_error(tr("noDspOutput"));
    wave->finish();
    runEncoder(device, extract, pcm, dest, abort);
    std::error_code ignored;
    fs::remove(pcm, ignored);
    pfc::string8 url;
    filesystem::g_get_canonical_path(utf(dest.wstring()).c_str(), url);
    input_info_writer::ptr writer;
    try {
        input_entry::g_open_for_info_write(writer, {}, url, abort);
    } catch (const exception_io_unsupported_format &) {
        if (!extract && device.output().custom())
            return tr("noTagWriter");
        throw;
    } catch (const exception_tagging_unsupported &) {
        if (!extract && device.output().custom())
            return tr("tagsUnsupported");
        throw;
    }
    file_info_impl tags;
    writer->get_info(0, tags, abort);
    tags.copy_meta(info);
    tags.meta_remove_field("CUESHEET");
    writer->set_info(0, tags, abort);
    writer->commit(abort);
    return {};
}
} // namespace wm
