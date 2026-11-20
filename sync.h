#pragma once
#include <foobar2000/SDK/foobar2000.h>
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace wm {
namespace fs = std::filesystem;
using S = std::string;
using Rows = std::vector<std::vector<S>>;
inline constexpr char defaultPattern[] =
    R"fb([$meta(album artist)\][$meta(album)\]$if($meta(title),[%tracknumber%.]$meta(title),%filename%))fb";
std::wstring wide(const S &);
S utf(const std::wstring &);
S guid(const GUID &);
S unique();
S serializeChain(const dsp_chain_config &);
void restoreChain(dsp_chain_config &, const S &);
S safeLeaf(S name);
S normalizeRelative(const S &path);
fs::path profile();
fs::path encoders();
void bytes(const fs::path &, const S &);
S read(const fs::path &);
void check(HRESULT result, const char *what);
void error(const std::exception &);
class Db {
    struct Impl;
    std::unique_ptr<Impl> p;

  public:
    Db();
    ~Db();
    Rows query(const char *sql, std::initializer_list<S> args = {});
};
struct Encoder {
    S id, name, executable, arguments, extension;
    int bits; // 0 selects the source precision for lossless output, -32 is float PCM.
    S parameter;
    int minimum = 0, maximum = 0, value = 0;
    S mode;
    std::map<S, int> otherValues;
    bool custom() const { return id.starts_with("custom:"); }
};
std::vector<Encoder> defaultEncoders();
struct CodecMode {
    S id, label, arguments, parameter;
    int minimum, maximum, initial;
};
const std::vector<CodecMode> &codecModes(const S &codec);
void selectMode(Encoder &, const S &);
void validateEncoder(const Encoder &);
struct Device {
    S id, name, location, root, pattern, policy, resampler;
    int quality = 127;
    S chain;
    bool downsample = true;
    unsigned rate44 = 44100, rate48 = 48000;
    S aacMode = "tvbr";
    int bitrate = 256;
    S format = "aac";
    std::vector<Encoder> formats = defaultEncoders();
    unsigned appleRate44 = 44100, appleRate48 = 48000;
    bool autoSync = true;
    bool usesFamilyResampler() const { return format == "aac" || downsample; }
    const Encoder &output() const;
};
std::vector<Device> devices();
void save(const Device &);
void clearDeviceData(const Device &, bool removeDevice);
bool transferring(const Device &);
struct Endpoint {
    S key, label;
};
std::vector<Endpoint> endpoints();
struct Stamp {
    uint64_t size;
    S modified;
    S key() const { return std::to_string(size) + ":" + modified; }
};
class Storage {
  public:
    virtual ~Storage() = default;
    virtual std::optional<Stamp> stat(const S &relative) = 0;
    virtual Stamp put(const S &relative, const fs::path &src, bool replace, abort_callback &) = 0;
    virtual void rename(const S &from, const S &to) = 0;
};
std::unique_ptr<Storage> connect(const Device &);
std::vector<Endpoint> portableEndpoints();
std::unique_ptr<Storage> portableStorage(const Device &);
struct Song {
    metadb_handle_ptr handle;
    S title, path, rel, identity, signature;
    S coverHash;
    int seconds = 0;
    enum Mode { copy, convert, extract } mode = copy;
};
struct List {
    S id, name;
    metadb_handle_list items;
};
Song::Mode transferMode(const Device &, const file_info &, unsigned subsong, const S &extension);
S outputExtension(const Device &, Song::Mode, const S &extension);
S encoderSignature(const Device &);
S encode(const Song &, const Device &, const fs::path &, abort_callback &);
void runEncoder(const Device &, bool extract, const fs::path &wav, const fs::path &dest, abort_callback &);
unsigned targetRate(const Device &, unsigned source);
std::vector<S> resamplers();
void start(const Device &, const std::set<S> &playlists = {}, bool automatic = false,
           metadb_handle_list_cref tracks = metadb_handle_list());
void poll();
bool online(const Device &);
void bridge();
std::set<S> chooseConflicts(const std::vector<S> &paths);
extern const GUID prefsId;
} // namespace wm
