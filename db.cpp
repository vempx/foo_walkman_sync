#include "sync.h"
#include <winsqlite/winsqlite3.h>
#include <sstream>
#pragma comment(lib, "winsqlite3.lib")
namespace wm {
std::wstring wide(const S &s) {
    if (s.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), nullptr, 0);
    if (!n)
        throw std::runtime_error("Invalid UTF-8");
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
S utf(const std::wstring &w) {
    if (w.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    S s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
S guid(const GUID &g) {
    wchar_t buf[40]{};
    StringFromGUID2(g, buf, 40);
    return utf(buf);
}
S unique() {
    GUID g;
    check(CoCreateGuid(&g), "Create GUID");
    return guid(g);
}
void check(HRESULT hr, const char *what) {
    if (FAILED(hr)) {
        std::ostringstream msg;
        msg << what << " (0x" << std::hex << (unsigned)hr << ")";
        throw std::runtime_error(msg.str());
    }
}
void error(const std::exception &e) { popup_message::g_show(e.what(), "Walkman Sync"); }
fs::path profile() {
    auto path = filesystem::g_get_native_path(core_api::get_profile_path());
    return fs::path(wide(path.c_str())) / L"walkman-sync";
}
fs::path encoders() {
    wchar_t exe[32768];
    auto n = GetModuleFileNameW(nullptr, exe, _countof(exe));
    return fs::path(std::wstring(exe, n)).parent_path() / L"encoders";
}
void bytes(const fs::path &path, const S &text) {
    std::ofstream out;
    out.exceptions(std::ios::badbit | std::ios::failbit);
    out.open(path, std::ios::binary);
    out.write(text.data(), text.size());
    out.close();
}
S read(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error("Cannot open " + utf(path.wstring()));
    return {std::istreambuf_iterator<char>(in), {}};
}
S safeLeaf(S s) {
    for (char &c : s)
        if ((unsigned char)c < 32 || S("<>:\"/\\|?*").find(c) != S::npos)
            c = '_';
    const auto first = s.find_first_not_of(' ');
    if (first != 0)
        s.erase(0, first);
    while (!s.empty() && (s.back() == '.' || s.back() == ' '))
        s.pop_back();
    if (s.empty())
        s = "_";
    auto stem = s.substr(0, s.find('.'));
    for (char &c : stem)
        if (c >= 'a' && c <= 'z')
            c -= 32;
    if (stem == "CON" || stem == "NUL" || stem == "AUX" || stem == "PRN" ||
        (stem.size() == 4 && (stem.starts_with("COM") || stem.starts_with("LPT")) &&
         isdigit((unsigned char)stem[3])))
        s = "_" + s;
    return s;
}
S normalizeRelative(const S &text) {
    if (text.empty())
        throw std::runtime_error("路径不能为空");
    S result;
    size_t begin = 0;
    for (;;) {
        auto end = text.find_first_of("/\\", begin);
        auto part = text.substr(begin, end - begin);
        if (part.empty() || part == "." || part == ".." || part.find(':') != S::npos)
            throw std::runtime_error("需要相对路径，不能包含盘符、空目录或 ..");
        if (!result.empty())
            result += "\\";
        result += safeLeaf(part);
        if (end == S::npos)
            break;
        begin = end + 1;
    }
    return result;
}
struct Db::Impl {
    sqlite3 *db = nullptr;
    ~Impl() {
        if (db)
            sqlite3_close(db);
    }
};
Db::Db() : p(std::make_unique<Impl>()) {
    auto folder = profile();
    fs::create_directories(folder);
    if (sqlite3_open16((folder / L"records.sqlite").c_str(), &p->db) != SQLITE_OK)
        throw std::runtime_error("不能打开本地传输记录");
    sqlite3_busy_timeout(p->db, 5000);
    const char *schema =
        "CREATE TABLE IF NOT EXISTS device(id TEXT PRIMARY KEY,name TEXT,location TEXT,root TEXT,pattern "
        "TEXT,encoder TEXT,resampler TEXT,quality INTEGER, chain TEXT);"
        "CREATE TABLE IF NOT EXISTS audio_options(device TEXT PRIMARY KEY,downsample INTEGER,rate44 "
        "INTEGER,"
        "rate48 INTEGER,mode TEXT,bitrate INTEGER);"
        "CREATE TABLE IF NOT EXISTS encoder_profile(device TEXT,id TEXT,name TEXT,exe TEXT,args TEXT,ext "
        "TEXT,"
        "bits INTEGER,value INTEGER,active INTEGER,PRIMARY KEY(device,id));"
        "CREATE TABLE IF NOT EXISTS apple_aac_rates(device TEXT PRIMARY KEY,rate44 INTEGER,rate48 INTEGER);"
        "CREATE TABLE IF NOT EXISTS connect_sync(device TEXT PRIMARY KEY,enabled INTEGER);"
        "CREATE TABLE IF NOT EXISTS codec_mode(device TEXT,codec TEXT,mode TEXT,value INTEGER,active INTEGER,"
        "PRIMARY KEY(device,codec,mode));"
        "CREATE TABLE IF NOT EXISTS report(device TEXT,finished TEXT,summary TEXT);"
        "CREATE TABLE IF NOT EXISTS subscription(device TEXT,list TEXT,automatic INTEGER DEFAULT 0,PRIMARY "
        "KEY(device,list));"
        "CREATE TABLE IF NOT EXISTS track(device TEXT,path TEXT COLLATE NOCASE,identity TEXT,signature "
        "TEXT,stamp TEXT,origin TEXT,PRIMARY KEY(device,path));"
        "CREATE TABLE IF NOT EXISTS exported(device TEXT,list TEXT,file TEXT COLLATE NOCASE,stamp "
        "TEXT,PRIMARY KEY(device,list));";
    if (sqlite3_exec(p->db, schema, nullptr, nullptr, nullptr) != SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(p->db));
}
Db::~Db() = default;
Rows Db::query(const char *sql, std::initializer_list<S> args) {
    sqlite3_stmt *raw = nullptr;
    if (sqlite3_prepare_v2(p->db, sql, -1, &raw, nullptr) != SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(p->db));
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(raw, sqlite3_finalize);
    int i = 0;
    for (auto &val : args)
        if (sqlite3_bind_text(raw, ++i, val.data(), (int)val.size(), SQLITE_TRANSIENT) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(p->db));
    Rows rows;
    int rc;
    while ((rc = sqlite3_step(raw)) == SQLITE_ROW) {
        auto &row = rows.emplace_back();
        for (int col = 0; col < sqlite3_column_count(raw); ++col) {
            auto *val = sqlite3_column_text(raw, col);
            row.emplace_back(val ? (const char *)val : "", sqlite3_column_bytes(raw, col));
        }
    }
    if (rc != SQLITE_DONE)
        throw std::runtime_error(sqlite3_errmsg(p->db));
    return rows;
}
std::vector<Device> devices() {
    std::vector<Device> out;
    Db db;
    for (auto &r : db.query(
             "SELECT d.id,name,location,root,pattern,encoder,resampler,quality,chain,"
             "coalesce(a.downsample,1),coalesce(a.rate44,44100),coalesce(a.rate48,48000),"
             "coalesce(mode,'tvbr'),coalesce(bitrate,256),min(coalesce(p.rate44,a.rate44,44100),44100),"
             "min(coalesce(p.rate48,a.rate48,48000),48000) FROM device d LEFT JOIN "
             "audio_options a ON d.id=a.device LEFT JOIN apple_aac_rates p ON d.id=p.device ORDER BY "
             "d.rowid")) {
        auto &d =
            out.emplace_back(Device{r[0], r[1], r[2], r[3], r[4], r[5] == "aac" ? "lossless" : r[5], r[6],
                                    std::stoi(r[7]), r[8], r[9] == "1", (unsigned)std::stoul(r[10]),
                                    (unsigned)std::stoul(r[11]), r[12], std::stoi(r[13])});
        d.appleRate44 = (unsigned)std::stoul(r[14]);
        d.appleRate48 = (unsigned)std::stoul(r[15]);
        auto master = db.query("SELECT enabled FROM connect_sync WHERE device=?", {d.id});
        d.autoSync = master.empty() || master.front()[0] == "1";
        auto savedModes = db.query("SELECT codec,mode,value,active FROM codec_mode WHERE device=?", {d.id});
        for (auto &f : d.formats) {
            for (const auto &row : savedModes)
                if (row[0] == f.id && row[3] == "1") selectMode(f, row[1]);
            for (const auto &row : savedModes)
                if (row[0] == f.id) f.otherValues[row[1]] = std::stoi(row[2]);
        }
        for (auto &row : db.query("SELECT id,name,exe,args,ext,bits,value,active FROM encoder_profile "
                                  "WHERE device=? ORDER BY rowid",
                                  {d.id})) {
            auto it = std::find_if(d.formats.begin(), d.formats.end(),
                                   [&](const auto &f) { return f.id == row[0]; });
            if (it != d.formats.end())
                it->value = std::stoi(row[6]);
            else if (row[0].starts_with("custom:"))
                d.formats.push_back({row[0], row[1], row[2], row[3], row[4], std::stoi(row[5]), {}});
            else
                continue;
            if (row[7] == "1")
                d.format = row[0];
        }
    }
    return out;
}
void save(const Device &d) {
    Db db;
    db.query("BEGIN IMMEDIATE");
    db.query("INSERT OR REPLACE INTO device VALUES(?,?,?,?,?,?,?,?,?)",
             {d.id, d.name, d.location, d.root, d.pattern, d.policy, d.resampler, std::to_string(d.quality),
              d.chain});
    db.query("INSERT OR REPLACE INTO audio_options VALUES(?,?,?,?,?,?)",
             {d.id, d.downsample ? "1" : "0", std::to_string(d.rate44), std::to_string(d.rate48), d.aacMode,
              std::to_string(d.bitrate)});
    db.query("DELETE FROM encoder_profile WHERE device=?", {d.id});
    db.query("INSERT OR REPLACE INTO connect_sync VALUES(?,?)", {d.id, d.autoSync ? "1" : "0"});
    db.query("DELETE FROM codec_mode WHERE device=?", {d.id});
    db.query("INSERT OR REPLACE INTO apple_aac_rates VALUES(?,?,?)",
             {d.id, std::to_string(d.appleRate44), std::to_string(d.appleRate48)});
    for (const auto &f : d.formats) {
        if (!f.mode.empty()) {
            auto values = f.otherValues;
            values[f.mode] = f.value;
            for (const auto &[mode, value] : values)
                db.query("INSERT INTO codec_mode VALUES(?,?,?,?,?)",
                         {d.id,f.id,mode,std::to_string(value),mode == f.mode ? "1" : "0"});
        }
        db.query("INSERT INTO encoder_profile VALUES(?,?,?,?,?,?,?,?,?)",
                 {d.id, f.id, f.name, f.executable, f.arguments, f.extension, std::to_string(f.bits),
                  std::to_string(f.value), d.format == f.id ? "1" : "0"});
    }
    db.query("COMMIT");
}
void clearDeviceData(const Device &device, bool removeDevice) {
    if (transferring(device)) throw std::runtime_error("请等待此设备的传输结束后再操作。");
    Db db;
    db.query("BEGIN IMMEDIATE");
    // Track and playlist mappings are scoped to a device and its music root.
    auto prefix = device.id + "\n";
    for (auto table : {"track","exported"}) {
        auto sql = S("DELETE FROM ") + table + " WHERE device=? OR instr(device,?)=1";
        db.query(sql.c_str(), {device.id, prefix});
    }
    db.query("DELETE FROM report WHERE device=?", {device.id});
    if (removeDevice) {
        for (auto table : {"subscription","encoder_profile","audio_options","apple_aac_rates","connect_sync","codec_mode"}) {
            auto sql = S("DELETE FROM ") + table + " WHERE device=?";
            db.query(sql.c_str(), {device.id});
        }
        db.query("DELETE FROM device WHERE id=?", {device.id});
    }
    db.query("COMMIT");
}
} // namespace wm
