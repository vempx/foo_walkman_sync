#include "sync.h"
#include <array>

namespace wm {
static std::vector<Endpoint> volumes() {
    std::vector<Endpoint> out;
    DWORD drives = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(drives & (1u << i)))
            continue;
        wchar_t root[] = L"A:\\";
        root[0] += (wchar_t)i;
        auto type = GetDriveTypeW(root);
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE)
            continue;
        wchar_t id[128]{}, label[256]{};
        if (!GetVolumeNameForVolumeMountPointW(root, id, _countof(id)))
            continue;
        if (!GetVolumeInformationW(root, label, _countof(label), nullptr, nullptr, nullptr, nullptr, 0))
            continue;
        out.push_back({"vol:" + utf(id), utf(root) + " " + utf(label)});
    }
    return out;
}
std::vector<Endpoint> endpoints() {
    auto out = volumes();
    try {
        auto portable = portableEndpoints();
        out.insert(out.end(), portable.begin(), portable.end());
    } catch (const std::exception &e) {
        console::print(e.what());
    }
    return out;
}
class Volume final : public Storage {
    fs::path base;
    fs::path checked(const S &name) const {
        auto rel = fs::path(wide(normalizeRelative(name)));
        auto result = base;
        for (const auto &part : rel) {
            result /= part;
            auto attr = GetFileAttributesW(result.c_str());
            if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_REPARSE_POINT))
                throw std::runtime_error(tr("reparsePoint"));
        }
        return result;
    }

  public:
    explicit Volume(const Device &device) {
        fs::path vol(wide(device.location.substr(4)));
        if (!GetVolumeInformationW(vol.c_str(), nullptr, 0, nullptr, nullptr, nullptr, nullptr, 0))
            throw std::runtime_error(tr("volumeOffline"));
        base = vol;
        base = checked(device.root);
    }
    std::optional<Stamp> stat(const S &rel) override {
        auto path = checked(rel);
        if (!fs::exists(path))
            return {};
        return Stamp{fs::file_size(path),
                     std::to_string(fs::last_write_time(path).time_since_epoch().count())};
    }
    Stamp put(const S &rel, const fs::path &src, bool replace, abort_callback &abort) override {
        auto dest = checked(rel);
        fs::create_directories(dest.parent_path());
        auto stage = dest.parent_path() / wide(".walkman-" + unique() + ".part");
        try {
            std::ifstream in(src, std::ios::binary);
            std::ofstream out(stage, std::ios::binary);
            out.exceptions(std::ios::badbit | std::ios::failbit);
            std::vector<char> buffer(256 * 1024);
            while (in) {
                abort.check();
                in.read(buffer.data(), buffer.size());
                out.write(buffer.data(), in.gcount());
            }
            if (!in.eof() || in.bad())
                throw std::runtime_error(tr("readFailed"));
            out.close();
            if (fs::file_size(stage) != fs::file_size(src))
                throw std::runtime_error(tr("stagedSize"));
            abort.check();
            if (!MoveFileExW(stage.c_str(), dest.c_str(),
                             MOVEFILE_WRITE_THROUGH | (replace ? MOVEFILE_REPLACE_EXISTING : 0)))
                check(HRESULT_FROM_WIN32(GetLastError()), "Commit device file");
        } catch (...) {
            std::error_code ec;
            fs::remove(stage, ec);
            throw;
        }
        return stat(rel).value();
    }
    void rename(const S &from, const S &to) override {
        if (!MoveFileExW(checked(from).c_str(), checked(to).c_str(), MOVEFILE_WRITE_THROUGH))
            check(HRESULT_FROM_WIN32(GetLastError()), "Rename playlist");
    }
};
std::unique_ptr<Storage> connect(const Device &d) {
    if (d.location.starts_with("vol:"))
        return std::make_unique<Volume>(d);
    return portableStorage(d);
}
} // namespace wm
