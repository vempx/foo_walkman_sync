#include "sync.h"
#include <sstream>
#include <atomic>
namespace wm {
const GUID prefsId = {0xbf436018, 0x3ed6, 0x4168, {0x9f, 0x77, 0x25, 0x33, 0xe6, 0x88, 0x11, 0xa2}};
namespace {
const GUID bridgeId = {0x348bd067, 0x929d, 0x48bf, {0xbf, 0x41, 0xee, 0xf6, 0x0b, 0x92, 0x3d, 0x1f}};
const GUID fileId = {0x1dc11df8,0xe2e7,0x488c,{0xb0,0x0e,0xde,0xa7,0xe5,0x09,0xd7,0x23}};
const GUID tracksGroup = {0x86573786,0x1760,0x49bc,{0x9a,0xaa,0x6c,0x38,0x95,0x8c,0x9f,0x1f}};
const GUID tracksCommand = {0x5a9d247f,0xe142,0x4eb9,{0xb6,0x7e,0x1d,0xaf,0x00,0xb2,0xf2,0x18}};
std::set<S> connected;
bool scanning = false, closing = false;
UINT_PTR timer = 0;
GUID asGuid(const S &id) {
    GUID g{};
    check(CLSIDFromString(wide(id).c_str(), &g), "Playlist ID");
    return g;
}
class SyncCommand : public mainmenu_node_command {
    Device device;

  public:
    SyncCommand(Device d) : device(std::move(d)) {}
    void get_display(pfc::string_base &text, t_uint32 &flags) override {
        text = "同步所有订阅播放列表";
        flags = online(device) ? 0 : mainmenu_commands::flag_disabled;
    }
    GUID get_guid() override { return asGuid(device.id); }
    void execute(service_ptr) override {
        try {
            start(device, {});
        } catch (const std::exception &e) {
            error(e);
        }
    }
};
class DeviceMenu : public mainmenu_node_group {
    Device device;

  public:
    DeviceMenu(Device d) : device(std::move(d)) {}
    void get_display(pfc::string_base &text, t_uint32 &flags) override {
        text = device.name.c_str();
        flags = 0;
    }
    size_t get_children_count() override { return 1; }
    mainmenu_node::ptr get_child(size_t) override { return fb2k::service_new<SyncCommand>(device); }
};
class SyncDevices : public mainmenu_node_group {
    std::vector<Device> list = devices();
  public:
    void get_display(pfc::string_base &text, t_uint32 &flags) override {
        text = "Walkman Sync"; flags = 0;
    }
    size_t get_children_count() override { return list.size(); }
    mainmenu_node::ptr get_child(size_t i) override { return fb2k::service_new<DeviceMenu>(list.at(i)); }
};
class FileCommands : public mainmenu_commands_v2 {

  public:
    t_uint32 get_command_count() override {
        return 2;
    }
    GUID get_command(t_uint32 i) override {
        return i ? fileId : bridgeId;
    }
    GUID get_parent() override { return mainmenu_groups::file; }
    void get_name(t_uint32 i, pfc::string_base &text) override {
        text = i ? "Walkman Sync" : "Walkman Sync Panel";
    }
    bool get_display(t_uint32 i, pfc::string_base &text, t_uint32 &flags) override {
        get_name(i, text);
        flags = i ? 0 : flag_defaulthidden;
        return true;
    }
    bool get_description(t_uint32, pfc::string_base &) override { return false; }
    void execute(t_uint32 i, service_ptr) override {
        try {
            if (!i)
                bridge();
        } catch (const std::exception &e) {
            error(e);
        }
    }
    bool is_command_dynamic(t_uint32 i) override { return i > 0; }
    mainmenu_node::ptr dynamic_instantiate(t_uint32 i) override {
        return fb2k::service_new<SyncDevices>();
    }
};
mainmenu_commands_factory_t<FileCommands> menuFactory;
contextmenu_group_popup_factory trackGroupFactory(tracksGroup, contextmenu_groups::root, "Walkman Sync");
class SendTracks : public contextmenu_item_node_leaf {
    Device device;
  public:
    explicit SendTracks(Device d) : device(std::move(d)) {}
    bool get_display_data(pfc::string_base &name,unsigned &flags,metadb_handle_list_cref,const GUID &) override {
        name = device.name.c_str();
        flags = online(device) ? 0 : FLAG_DISABLED_GRAYED;
        return true;
    }
    GUID get_guid() override { return asGuid(device.id); }
    bool is_mappable_shortcut() override { return true; }
    bool get_description(pfc::string_base &text) override { text = "将所选曲目传输到设备"; return true; }
    void execute(metadb_handle_list_cref items,const GUID &) override {
        try { if (items.get_count()) start(device, {}, false, items); }
        catch (const std::exception &e) { error(e); }
    }
};
class Destinations : public contextmenu_item_node_root_popup {
    std::vector<SendTracks> children;
  public:
    Destinations() { for (auto &d : devices()) children.emplace_back(std::move(d)); }
    bool get_display_data(pfc::string_base &name,unsigned &flags,metadb_handle_list_cref,const GUID &) override {
        name = "发送至"; flags = 0; return true;
    }
    size_t get_children_count() override { return children.size(); }
    contextmenu_item_node *get_child(size_t i) override { return &children.at(i); }
    GUID get_guid() override { return pfc::guid_null; }
    bool is_mappable_shortcut() override { return false; }
};
class TrackCommands : public contextmenu_item_v2 {
  public:
    unsigned get_num_items() override { return 1; }
    GUID get_parent() override { return tracksGroup; }
    GUID get_item_guid(unsigned) override { return tracksCommand; }
    void get_item_name(unsigned,pfc::string_base &out) override { out = "发送至"; }
    bool get_item_description(unsigned,pfc::string_base &out) override { out = "发送所选曲目"; return true; }
    t_enabled_state get_enabled_state(unsigned) override { return DEFAULT_ON; }
    contextmenu_item_node_root *instantiate_item(unsigned,metadb_handle_list_cref,const GUID &) override {
        return new Destinations;
    }
    void item_execute_simple(unsigned,const GUID &id,metadb_handle_list_cref items,const GUID &caller) override {
        Destinations root;
        for (size_t i = 0; i < root.get_children_count(); ++i) {
            auto node = root.get_child(i);
            if (node->get_guid() == id) { node->execute(items,caller); return; }
        }
    }
};
contextmenu_item_factory_t<TrackCommands> trackFactory;
class Events : public initquit {
  public:
    void on_init() override {
        closing = false;
        timer = SetTimer(nullptr, 0, 4000, [](HWND, UINT, UINT_PTR, DWORD) {
            try {
                poll();
            } catch (const std::exception &e) {
                console::print(e.what());
            }
        });
        try {
            fs::create_directories(profile());
            poll();
        } catch (const std::exception &e) {
            console::print(e.what());
        }
    }
    void on_quit() override {
        closing = true;
        if (timer)
            KillTimer(nullptr, timer);
    }
};
initquit_factory_t<Events> events;
} // namespace
bool online(const Device &d) { return connected.count(d.location) != 0; }
void poll() {
    if (closing || scanning)
        return;
    auto configured = devices();
    if (configured.empty())
        return;
    scanning = true;
    fb2k::splitTask([] {
        HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        std::set<S> now;
        bool success = true;
        try {
            for (auto &end : endpoints())
                now.insert(end.key);
        } catch (...) {
            success = false;
        }
        if (SUCCEEDED(com))
            CoUninitialize();
        fb2k::inMainThread([now = std::move(now), success] {
            scanning = false;
            if (closing || !success)
                return;
            auto before = std::move(connected);
            connected = now;
            try {
                for (auto &d : devices())
                    if (online(d) && !before.contains(d.location))
                        start(d, {}, true);
            } catch (const std::exception &e) {
                console::print(e.what());
            }
        });
    });
}
void bridge() {
    auto dir = profile();
    auto path = dir / L"panel-request.txt";
    if (!fs::exists(path))
        return;
    auto body = read(path);
    fs::remove(path);
    if (body.starts_with("\xef\xbb\xbf"))
        body.erase(0, 3);
    body.erase(std::remove(body.begin(), body.end(), '\r'), body.end());
    std::istringstream in(body);
    S action, nonce, index, name, id, dev;
    std::getline(in, action);
    std::getline(in, nonce);
    std::getline(in, index);
    std::getline(in, name);
    std::getline(in, id);
    std::getline(in, dev);
    auto pm = playlist_manager_v5::get();
    bool batch = action.ends_with("2");
    if (batch) action.pop_back();
    std::set<S> keys;
    S keyList, part;
    std::istringstream indices(index);
    while (std::getline(indices, part, ',')) {
        auto i = std::stoull(part);
        if (i >= pm->get_playlist_count()) throw std::runtime_error("歌单已经移除");
        if (!batch) {
            pfc::string8 actual;
            pm->playlist_get_name(i, actual);
            if (actual != name.c_str()) throw std::runtime_error("歌单已变化，请重新打开菜单");
        }
        auto key = guid(pm->playlist_get_guid(i));
        keys.insert(key);
        if (!keyList.empty()) keyList += ",";
        keyList += key;
    }
    if (keys.empty()) return;
    auto all = devices();
    Db db;
    if (action == "list") {
        S reply = nonce + "\n" + keyList + "\n";
        for (auto &d : all) {
            auto sub = db.query("SELECT list FROM subscription WHERE device=?", {d.id});
            size_t matched = std::count_if(sub.begin(), sub.end(), [&](const auto &r) {return keys.contains(r[0]);});
            reply += d.id + "\t" + d.name + "\t" + (online(d) ? "1" : "0") + "\t" +
                     (matched == keys.size() ? "1" : "0") + "\n";
        }
        bytes(dir / L"panel-response.txt", reply);
        return;
    }
    if (keyList != id)
        throw std::runtime_error("歌单已变化，请重新打开菜单");
    for (auto &d : all)
        if (dev == d.id) {
            if (action == "send")
                start(d, keys);
            else if (action == "subscribe" || action == "unsubscribe") {
                db.query("BEGIN IMMEDIATE");
                for (auto &key : keys)
                    if (action == "subscribe")
                        db.query("INSERT OR IGNORE INTO subscription(device,list) VALUES(?,?)", {dev, key});
                    else
                        db.query("DELETE FROM subscription WHERE device=? AND list=?", {dev, key});
                db.query("COMMIT");
            }
            return;
        }
}
} // namespace wm
