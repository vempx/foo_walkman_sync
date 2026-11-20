#include "sync.h"
#include <cmath>
#include <sstream>
#include <foobar2000/SDK/album_art.h>
#include <foobar2000/SDK/hasher_md5.h>

namespace wm {
Song::Mode transferMode(const Device &device, const file_info &info, unsigned subsong, const S &extension) {
    const char *encoding = info.info_get("encoding");
    if (device.policy == "all" ||
        (device.policy == "lossless" && encoding && !_stricmp(encoding, "lossless")))
        return Song::convert;
    if (subsong || !_stricmp(extension.c_str(), ".cue"))
        return Song::extract;
    return Song::copy;
}
S outputExtension(const Device &device, Song::Mode mode, const S &extension) {
    if (mode == Song::convert)
        return "." + device.output().extension;
    if (mode == Song::extract)
        return ".flac";
    return extension.empty() ? S{} : safeLeaf(extension);
}
namespace {
std::set<S> busy;
S stampSource(const fs::path &path) {
    return std::to_string(fs::file_size(path)) + ":" +
           std::to_string(fs::last_write_time(path).time_since_epoch().count());
}
album_art_data_ptr transferCover(const Song &s, abort_callback &abort) {
    const auto &front = album_art_ids::cover_front;
    if (s.mode == Song::copy) {
        auto path = s.handle->get_path();
        if (!album_art_editor::g_is_supported_path(path))
            return {};
        try {
            auto embedded = album_art_extractor::g_open({}, path, abort);
            if (embedded->have_entry(front, abort))
                return {};
        } catch (const exception_album_art_not_found &) {
        }
    }
    try {
        auto art = album_art_manager_v2::get()->open(pfc::list_single_ref_t<metadb_handle_ptr>(s.handle),
                                                     pfc::list_single_ref_t<GUID>(front), abort);
        return art->query(front, abort);
    } catch (const exception_album_art_not_found &) {
        return {};
    }
}
S coverHash(const album_art_data_ptr &art) {
    return hasher_md5::get()->process_single(art->data(), art->size()).asString().c_str();
}
Song prepare(metadb_handle_ptr h, const Device &device, titleformat_object::ptr pattern,
             abort_callback &abort) {
    Song s;
    s.handle = h;
    auto sub = h->get_subsong_index();
    auto path = filesystem::g_get_native_path(h->get_path());
    s.path = path.c_str();
    if (s.path.empty())
        throw std::runtime_error("仅支持本地源文件");
    auto src = fs::path(wide(s.path));
    auto ext = utf(src.extension().wstring());
    s.identity = s.path + "|" + std::to_string(sub);
    file_info_impl info;
    if (!h->get_info(info)) {
        input_info_reader::ptr reader;
        input_entry::g_open_for_info_read(reader, {}, h->get_path(), fb2k::noAbort);
        reader->get_info(sub, info, fb2k::noAbort);
    }
    bool cue = !_stricmp(ext.c_str(), ".cue");
    s.mode = transferMode(device, info, sub, ext);
    s.signature = stampSource(src);
    if (cue) {
        std::istringstream lines(read(src));
        S line;
        while (std::getline(lines, line)) {
            auto begin = line.find_first_not_of(" \t");
            if (begin == S::npos || _strnicmp(line.c_str() + begin, "FILE ", 5))
                continue;
            auto value = line.substr(begin + 5);
            auto quoted = value.find('"');
            S ref;
            if (quoted != S::npos) {
                auto end = value.find('"', quoted + 1);
                if (end != S::npos)
                    ref = value.substr(quoted + 1, end - quoted - 1);
            } else
                ref = value.substr(0, value.find_first_of(" \t\r"));
            if (!ref.empty())
                s.signature += "|" + stampSource(src.parent_path() / wide(ref));
        }
    }
    if (s.mode == Song::convert) {
        s.signature += "|" + encoderSignature(device) + ":" + device.chain;
        auto rate = (unsigned)info.info_get_int("samplerate");
        const auto target = targetRate(device, rate);
        if (target != rate) {
            s.signature += "|" + device.resampler;
            // Preserve 0.1 records when the effective rate and resampler still match.
            const auto oldTarget = rate <= 48000 ? rate : rate % 44100 == 0 ? 44100u : 48000u;
            if (target != oldTarget)
                s.signature += ":" + std::to_string(target);
        }
    }
    if (s.mode == Song::extract) {
        s.signature += "|flac";
    }
    if (auto cover = transferCover(s, abort)) {
        s.coverHash = coverHash(cover);
        s.signature += "|front:" + s.coverHash;
    }
    pfc::string8 formatted;
    titleformat_text_filter_impl_filename_chars filter;
    h->format_title_from_external_info(info, nullptr, formatted, pattern, &filter);
    s.rel = normalizeRelative(formatted.c_str()) + outputExtension(device, s.mode, ext);
    auto title = info.meta_get("title", 0);
    s.title = title ? title : utf(src.stem().wstring());
    for (auto &c : s.title)
        if (c == '\r' || c == '\n')
            c = ' ';
    s.seconds = (int)std::lround(info.get_length());
    return s;
}
struct Temp {
    fs::path dir = fs::temp_directory_path() / wide("fb2k-walkman-" + unique());
    Temp() { fs::create_directory(dir); }
    ~Temp() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};
struct IgnoreCase {
    bool operator()(const S &a, const S &b) const {
        return CompareStringOrdinal(wide(a).c_str(), -1, wide(b).c_str(), -1, TRUE) == CSTR_LESS_THAN;
    }
};
struct Entry {
    Song song;
    bool ok = false;
    std::optional<Stamp> conflict;
};
struct Manifest {
    S id, name;
    std::vector<Entry *> order;
};
class Transfer : public threaded_process_callback {
  public:
    Device device;
    std::vector<List> lists;
    S report;
    size_t reused = 0, written = 0, playlists = 0;
    Transfer(Device d) : device(std::move(d)) {}
    void work(threaded_process_status &status, abort_callback &abort) {
        auto target = connect(device);
        Db db;
        auto scope = device.id + "\n" + device.root;
        titleformat_object::ptr pattern;
        if (!titleformat_compiler::get()->compile(pattern, device.pattern.c_str()))
            throw std::runtime_error("无效的标题格式模板");
        std::map<S, Entry, IgnoreCase> entries;
        std::vector<Manifest> manifests;
        auto problem = [&](const S &label, const std::exception &e) {
            report += label + " — " + e.what() + "\n";
        };
        for (auto &list : lists) {
            Manifest *manifest = list.id.empty() ? nullptr :
                &manifests.emplace_back(Manifest{list.id, safeLeaf(list.name) + ".m3u8", {}});
            for (auto h : list.items) {
                abort.check();
                try {
                    auto song = prepare(h, device, pattern, abort);
                    auto rel = song.rel;
                    auto it = entries.find(rel);
                    if (it == entries.end())
                        it = entries.emplace(rel, Entry{std::move(song)}).first;
                    else if (it->second.song.identity != song.identity)
                        throw std::runtime_error("两首源曲目指向同一个目标：" + rel);
                    if (manifest) manifest->order.push_back(&it->second);
                } catch (const exception_aborted &) {
                    throw;
                } catch (const std::exception &e) {
                    problem(h->get_path(), e);
                }
            }
        }
        auto record = [&](Entry &e, const Stamp &stamp, const char *origin) {
            auto &s = e.song;
            db.query("INSERT OR REPLACE INTO track VALUES(?,?,?,?,?,?)",
                     {scope, s.rel, s.identity, s.signature, stamp.key(), origin});
            e.ok = true;
        };
        auto send = [&](Entry &e, bool replace) {
            auto &s = e.song;
            Temp tmp;
            fs::path src = wide(s.path);
            if (s.mode != Song::copy) {
                src = tmp.dir / wide("track" + outputExtension(device, s.mode, {}));
                auto warning = encode(s, device, src, abort);
                if (!warning.empty())
                    report += s.rel + " — " + warning + "\n";
            }
            if (!s.coverHash.empty()) {
                auto cover = transferCover(s, abort);
                if (!cover || coverHash(cover) != s.coverHash)
                    throw std::runtime_error("源封面在同步期间发生变化");
                if (s.mode == Song::copy) {
                    auto staged = tmp.dir / (L"track" + src.extension().wstring());
                    fs::copy_file(src, staged);
                    fs::permissions(staged, fs::perms::owner_write, fs::perm_options::add);
                    src = staged;
                }
                pfc::string8 url;
                filesystem::g_get_canonical_path(utf(src.wstring()).c_str(), url);
                if (s.mode == Song::convert && device.output().custom() &&
                    !album_art_editor::g_is_supported_path(url)) {
                    report += s.rel + " — 此格式没有可用的封面写入器\n";
                } else {
                    auto editor = album_art_editor::g_open({}, url, abort);
                    editor->set(album_art_ids::cover_front, cover, abort);
                    editor->commit(abort);
                }
            }
            auto before = target->stat(s.rel);
            if (replace ? (!before || !e.conflict || before->key() != e.conflict->key())
                        : before.has_value())
                throw std::runtime_error("设备文件在同步期间发生变化");
            auto stamp = target->put(s.rel, src, replace, abort);
            record(e, stamp, "sent");
            ++written;
        };
        std::vector<S> conflicts;
        size_t i = 0;
        for (auto &[rel, e] : entries) {
            abort.check();
            status.set_progress(i++, entries.size() + 1);
            status.set_item(rel.c_str());
            try {
                auto stamp = target->stat(rel);
                if (!stamp) {
                    send(e, false);
                    continue;
                }
                auto &s = e.song;
                auto history = db.query(
                    "SELECT identity,signature,stamp FROM track WHERE device=? AND path=?", {scope, rel});
                if (!history.empty() &&
                    history[0] == std::vector<S>{s.identity, s.signature, stamp->key()}) {
                    e.ok = true;
                    ++reused;
                    continue;
                }
                e.conflict = stamp;
                conflicts.push_back(rel);
            } catch (const exception_aborted &) {
                throw;
            } catch (const std::exception &ex) {
                problem(rel, ex);
            }
        }
        std::set<S> overwrite;
        if (!conflicts.empty())
            fb2k::inMainThreadSynchronous2([&] { overwrite = chooseConflicts(conflicts); });
        for (auto &rel : conflicts) {
            abort.check();
            auto &e = entries.at(rel);
            status.set_item(rel.c_str());
            try {
                if (overwrite.count(rel))
                    send(e, true);
                else {
                    auto stamp = target->stat(rel);
                    if (!stamp || stamp->key() != e.conflict->key())
                        throw std::runtime_error("设备文件已变化，未沿用");
                    record(e, *stamp, "adopted");
                    ++reused;
                }
            } catch (const exception_aborted &) {
                throw;
            } catch (const std::exception &ex) {
                problem(rel, ex);
            }
        }
        for (auto &m : manifests) {
            abort.check();
            try {
                auto history =
                    db.query("SELECT file,stamp FROM exported WHERE device=? AND list=?", {scope, m.id});
                auto owners = db.query(
                    "SELECT list FROM exported WHERE device=? AND file=? COLLATE NOCASE AND list<>?",
                    {scope, m.name, m.id});
                if (!owners.empty())
                    throw std::runtime_error("另一张播放列表已使用相同文件名");
                S content = "#EXTM3U\r\n";
                for (auto *item : m.order)
                    if (item->ok) {
                        auto &s = item->song;
                        content += "#EXTINF:" + std::to_string(s.seconds) + "," + s.title + "\r\n" + s.rel +
                                   "\r\n";
                    }
                auto existing = target->stat(m.name);
                bool owned =
                    !history.empty() && CompareStringOrdinal(wide(history[0][0]).c_str(), -1,
                                                             wide(m.name).c_str(), -1, TRUE) == CSTR_EQUAL;
                if (!history.empty() && !owned && !existing) {
                    auto old = target->stat(history[0][0]);
                    if (old && old->key() == history[0][1]) {
                        target->rename(history[0][0], m.name);
                        existing = target->stat(m.name);
                        owned = true;
                    }
                }
                if (existing && !owned) {
                    bool approved = false;
                    fb2k::inMainThreadSynchronous2([&] {
                        approved = MessageBoxW(core_api::get_main_window(),
                                               wide("覆盖设备已有歌单 " + m.name + "？").c_str(),
                                               L"Walkman Sync", MB_YESNO | MB_DEFBUTTON2) == IDYES;
                    });
                    if (!approved)
                        continue;
                }
                Temp tmp;
                auto src = tmp.dir / L"playlist.m3u8";
                bytes(src, content);
                auto done = target->put(m.name, src, existing.has_value(), abort);
                db.query("INSERT OR REPLACE INTO exported VALUES(?,?,?,?)",
                         {scope, m.id, m.name, done.key()});
                ++playlists;
            } catch (const exception_aborted &) {
                throw;
            } catch (const std::exception &e) {
                problem(m.name, e);
            }
        }
    }
    void run(threaded_process_status &status, abort_callback &abort) override {
        HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        try {
            work(status, abort);
        } catch (const std::exception &e) {
            report += e.what();
        }
        if (SUCCEEDED(com))
            CoUninitialize();
    }
    void on_done(HWND, bool cancelled) override {
        busy.erase(device.location);
        S summary = device.name + (cancelled ? " 已取消" : " 同步结束") + "\n发送 " +
                    std::to_string(written) + " 首，复用 " + std::to_string(reused) + " 首，写入 " +
                    std::to_string(playlists) + " 张歌单。";
        if (!report.empty())
            summary += "\n\n" + report;
        try {
            Db db;
            db.query("INSERT INTO report VALUES(?,datetime('now','localtime'),?)", {device.id, summary});
            db.query("DELETE FROM report WHERE device=? AND rowid NOT IN "
                     "(SELECT rowid FROM report WHERE device=? ORDER BY rowid DESC LIMIT 20)",
                     {device.id, device.id});
        } catch (const std::exception &e) {
            console::print(e.what());
        }
        popup_message::g_show(summary.c_str(), "Walkman Sync");
    }
};
} // namespace
bool transferring(const Device &device) { return busy.contains(device.location); }
void start(const Device &device, const std::set<S> &playlists, bool automatic, metadb_handle_list_cref tracks) {
    if (automatic && !device.autoSync) return;
    if (busy.contains(device.location)) {
        if (!automatic)
            popup_message::g_show("设备正在同步。", "Walkman Sync");
        return;
    }
    auto task = fb2k::service_new<Transfer>(device);
    auto pm = playlist_manager_v5::get();
    std::set<S> subscribed = playlists;
    if (tracks.get_count())
        task->lists.emplace_back().items.add_items(tracks);
    else if (playlists.empty())
        for (auto &row :
             Db().query("SELECT list FROM subscription WHERE device=? AND (automatic=1 OR ?='all')",
                        {device.id, automatic ? "auto" : "all"}))
            subscribed.insert(row[0]);
    for (size_t i = 0; i < pm->get_playlist_count(); ++i) {
        auto id = guid(pm->playlist_get_guid(i));
        if (!subscribed.contains(id))
            continue;
        auto &list = task->lists.emplace_back();
        list.id = id;
        pfc::string8 name;
        pm->playlist_get_name(i, name);
        list.name = name.c_str();
        pm->playlist_get_all_items(i, list.items);
    }
    if (task->lists.empty()) {
        if (!automatic)
            popup_message::g_show("设备没有可同步的订阅歌单。", "Walkman Sync");
        return;
    }
    busy.insert(device.location);
    try {
        if (!threaded_process::g_run_modeless(task,
                                              threaded_process::flag_show_item |
                                                  threaded_process::flag_show_abort |
                                                  threaded_process::flag_show_progress,
                                              core_api::get_main_window(), device.name.c_str()))
            throw std::runtime_error("无法启动传输");
    } catch (...) {
        busy.erase(device.location);
        throw;
    }
}
} // namespace wm
