#include "sync.h"
#include <atlbase.h>
#include <PortableDeviceApi.h>
#include <PortableDevice.h>
#include <propvarutil.h>

namespace wm {
namespace {
template <class T> CComPtr<T> make(REFCLSID cls) {
    CComPtr<T> obj;
    check(obj.CoCreateInstance(cls), "Create WPD service");
    return obj;
}
using Values = CComPtr<IPortableDeviceValues>;
std::optional<uint64_t> sizeValue(IPortableDeviceValues *values, REFPROPERTYKEY key) {
    ULONGLONG large;
    if (SUCCEEDED(values->GetUnsignedLargeIntegerValue(key, &large)))
        return large;
    DWORD size32;
    if (SUCCEEDED(values->GetUnsignedIntegerValue(key, &size32)))
        return size32;
    return {};
}
S stringValue(IPortableDeviceValues *values, REFPROPERTYKEY key) {
    LPWSTR p = nullptr;
    if (FAILED(values->GetStringValue(key, &p)))
        return {};
    std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> text(p, CoTaskMemFree);
    return utf(p);
}
void str(IPortableDeviceValues *values, REFPROPERTYKEY key, const S &value) {
    check(values->SetStringValue(key, wide(value).c_str()), "Set object property");
}
struct Obj {
    S id, name, stable, time;
    uint64_t size{};
    GUID type{};
    bool hasSize = false;
};
class Session {
  public:
    CComPtr<IPortableDevice> device;
    CComPtr<IPortableDeviceContent> content;
    CComPtr<IPortableDeviceProperties> properties;
    CComPtr<IPortableDeviceResources> resources;
    CComPtr<IPortableDeviceKeyCollection> keys;
    Session(const S &pnp, bool write) {
        device = make<IPortableDevice>(CLSID_PortableDeviceFTM);
        auto client = make<IPortableDeviceValues>(CLSID_PortableDeviceValues);
        str(client, WPD_CLIENT_NAME, "foobar2000 Walkman Sync");
        client->SetUnsignedIntegerValue(WPD_CLIENT_DESIRED_ACCESS,
                                        write ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ);
        check(device->Open(wide(pnp).c_str(), client), "Open MTP device");
        check(device->Content(&content), "MTP content");
        check(content->Properties(&properties), "MTP properties");
        check(content->Transfer(&resources), "MTP resources");
        keys = make<IPortableDeviceKeyCollection>(CLSID_PortableDeviceKeyCollection);
        for (auto key : {WPD_OBJECT_NAME, WPD_OBJECT_ORIGINAL_FILE_NAME, WPD_OBJECT_SIZE,
                         WPD_OBJECT_DATE_MODIFIED, WPD_OBJECT_PERSISTENT_UNIQUE_ID, WPD_OBJECT_CONTENT_TYPE,
                         WPD_FUNCTIONAL_OBJECT_CATEGORY, WPD_STORAGE_SERIAL_NUMBER})
            keys->Add(key);
    }
    Values values(const S &id) {
        Values v;
        check(properties->GetValues(wide(id).c_str(), keys, &v), "Read MTP properties");
        return v;
    }
    std::vector<S> children(const S &id) {
        CComPtr<IEnumPortableDeviceObjectIDs> it;
        check(content->EnumObjects(0, wide(id).c_str(), nullptr, &it), "List MTP folder");
        std::vector<S> result;
        for (;;) {
            LPWSTR ids[32]{};
            DWORD n = 0;
            check(it->Next(32, ids, &n), "Read MTP listing");
            for (DWORD i = 0; i < n; ++i) {
                result.push_back(utf(ids[i]));
                CoTaskMemFree(ids[i]);
            }
            if (n < 32)
                break;
        }
        return result;
    }
    Obj object(const S &id) {
        auto v = values(id);
        Obj obj;
        obj.id = id;
        obj.name = stringValue(v, WPD_OBJECT_ORIGINAL_FILE_NAME);
        if (obj.name.empty())
            obj.name = stringValue(v, WPD_OBJECT_NAME);
        obj.stable = stringValue(v, WPD_OBJECT_PERSISTENT_UNIQUE_ID);
        if (auto n = sizeValue(v, WPD_OBJECT_SIZE)) {
            obj.size = *n;
            obj.hasSize = true;
        }
        v->GetGuidValue(WPD_OBJECT_CONTENT_TYPE, &obj.type);
        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (SUCCEEDED(v->GetValue(WPD_OBJECT_DATE_MODIFIED, &pv))) {
            if (pv.vt == VT_DATE)
                obj.time = std::to_string(pv.date);
            if (pv.vt == VT_FILETIME)
                obj.time = std::to_string(pv.filetime.dwHighDateTime) + ":" +
                           std::to_string(pv.filetime.dwLowDateTime);
        }
        PropVariantClear(&pv);
        return obj;
    }
    std::optional<Obj> find(const S &parent, const S &name) {
        auto w = wide(name);
        for (auto &id : children(parent)) {
            auto obj = object(id);
            if (CompareStringOrdinal(wide(obj.name).c_str(), -1, w.c_str(), -1, TRUE) == CSTR_EQUAL)
                return obj;
        }
        return {};
    }
    S folder(const S &parent, const S &name) {
        auto v = make<IPortableDeviceValues>(CLSID_PortableDeviceValues);
        str(v, WPD_OBJECT_PARENT_ID, parent);
        str(v, WPD_OBJECT_NAME, name);
        str(v, WPD_OBJECT_ORIGINAL_FILE_NAME, name);
        v->SetGuidValue(WPD_OBJECT_CONTENT_TYPE, WPD_CONTENT_TYPE_FOLDER);
        v->SetGuidValue(WPD_OBJECT_FORMAT, WPD_OBJECT_FORMAT_PROPERTIES_ONLY);
        LPWSTR object = nullptr;
        check(content->CreateObjectWithPropertiesOnly(v, &object), "Create MTP folder");
        auto id = utf(object);
        CoTaskMemFree(object);
        return id;
    }
    void renameObject(const S &id, const S &name) {
        auto v = make<IPortableDeviceValues>(CLSID_PortableDeviceValues);
        str(v, WPD_OBJECT_ORIGINAL_FILE_NAME, name);
        str(v, WPD_OBJECT_NAME, name);
        Values errors;
        check(properties->SetValues(wide(id).c_str(), v, &errors), "Rename MTP object");
        if (object(id).name != name)
            throw std::runtime_error(tr("renameRejected"));
    }
    void discard(const S &id) {
        auto ids = make<IPortableDevicePropVariantCollection>(CLSID_PortableDevicePropVariantCollection);
        CComVariant value(wide(id).c_str());
        PROPVARIANT pv{};
        pv.vt = VT_LPWSTR;
        pv.pwszVal = value.bstrVal;
        check(ids->Add(&pv), "Select temporary file");
        check(content->Delete(PORTABLE_DEVICE_DELETE_NO_RECURSION, ids, nullptr), "Remove temporary file");
    }
    std::optional<uint64_t> resourceSize(const S &id) {
        Values attrs;
        if (SUCCEEDED(resources->GetResourceAttributes(wide(id).c_str(), WPD_RESOURCE_DEFAULT, &attrs)))
            return sizeValue(attrs, WPD_RESOURCE_ATTRIBUTE_TOTAL_SIZE);
        return {};
    }
    uint64_t readBack(const S &id, abort_callback &abort, const fs::path *source = nullptr) {
        CComPtr<IStream> input;
        DWORD optimal = 0;
        check(resources->GetStream(wide(id).c_str(), WPD_RESOURCE_DEFAULT, STGM_READ, &optimal, &input),
              "Open MTP readback");
        std::ifstream local;
        if (source) {
            local.open(*source, std::ios::binary);
            if (!local)
                throw std::runtime_error(tr("verifyOpen"));
        }
        std::vector<char> remote(std::clamp<DWORD>(optimal, 4096, 1024 * 1024)), expected(remote.size());
        uint64_t total = 0;
        for (;;) {
            abort.check();
            ULONG got = 0;
            auto hr = input->Read(remote.data(), (ULONG)remote.size(), &got);
            check(hr, "Read MTP data for verification");
            if (source && got) {
                local.read(expected.data(), got);
                if (local.gcount() != got || memcmp(remote.data(), expected.data(), got))
                    throw std::runtime_error(message("verifyMismatch", {std::to_string(total)}));
            }
            total += got;
            if (!got || hr == S_FALSE)
                return total;
        }
    }
    Stamp stamp(const Obj &obj, uint64_t size) { return {size, obj.stable + ":" + obj.time}; }
    Obj upload(const S &parent, const S &name, const fs::path &src, abort_callback &abort) {
        auto v = make<IPortableDeviceValues>(CLSID_PortableDeviceValues);
        str(v, WPD_OBJECT_PARENT_ID, parent);
        str(v, WPD_OBJECT_NAME, name);
        str(v, WPD_OBJECT_ORIGINAL_FILE_NAME, name);
        const auto expected = fs::file_size(src);
        check(v->SetUnsignedLargeIntegerValue(WPD_OBJECT_SIZE, expected), "Set MTP file size");
        auto ext = fs::path(wide(name)).extension().wstring();
        GUID fmt = WPD_OBJECT_FORMAT_UNSPECIFIED, kind = WPD_CONTENT_TYPE_GENERIC_FILE;
        if (!_wcsicmp(ext.c_str(), L".flac"))
            fmt = WPD_OBJECT_FORMAT_FLAC;
        if (!_wcsicmp(ext.c_str(), L".m4a"))
            fmt = WPD_OBJECT_FORMAT_M4A;
        if (!_wcsicmp(ext.c_str(), L".mp3"))
            fmt = WPD_OBJECT_FORMAT_MP3;
        if (!_wcsicmp(ext.c_str(), L".ogg"))
            fmt = WPD_OBJECT_FORMAT_OGG;
        if (fmt != WPD_OBJECT_FORMAT_UNSPECIFIED)
            kind = WPD_CONTENT_TYPE_AUDIO;
        v->SetGuidValue(WPD_OBJECT_FORMAT, fmt);
        v->SetGuidValue(WPD_OBJECT_CONTENT_TYPE, kind);
        CComPtr<IStream> stream;
        DWORD optimal = 0;
        auto result = content->CreateObjectWithPropertiesAndData(v, &stream, &optimal, nullptr);
        if (FAILED(result) && !stream && fmt != WPD_OBJECT_FORMAT_UNSPECIFIED &&
            (result == E_INVALIDARG || result == E_NOTIMPL ||
             result == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED))) {
            v->SetGuidValue(WPD_OBJECT_FORMAT, WPD_OBJECT_FORMAT_UNSPECIFIED);
            v->SetGuidValue(WPD_OBJECT_CONTENT_TYPE, WPD_CONTENT_TYPE_GENERIC_FILE);
            result = content->CreateObjectWithPropertiesAndData(v, &stream, &optimal, nullptr);
        }
        check(result, "Create MTP file");
        HRESULT writeError = S_OK;
        uint64_t offset = 0;
        try {
            std::ifstream input(src, std::ios::binary);
            std::vector<char> buffer(optimal ? std::min<DWORD>(optimal, 1024 * 1024) : 64 * 1024);
            while (input) {
                abort.check();
                input.read(buffer.data(), buffer.size());
                auto n = (ULONG)input.gcount();
                if (!n)
                    break;
                ULONG sent = 0;
                while (sent < n) {
                    abort.check();
                    ULONG done = 0;
                    writeError = stream->Write(buffer.data() + sent, n - sent, &done);
                    if (FAILED(writeError)) {
                        auto detail = message("writeOffset", {std::to_string(offset), std::to_string(expected)});
                        check(writeError, detail.c_str());
                    }
                    if (!done || done > n - sent)
                        throw std::runtime_error(tr("writeUnconfirmed"));
                    sent += done;
                    offset += done;
                }
            }
            if (!input.eof() || input.bad())
                throw std::runtime_error(tr("readFailed"));
            abort.check();
            check(stream->Commit(STGC_DEFAULT), "Commit MTP file");
        } catch (...) {
            stream->Revert();
            stream.Release();
            if (FAILED(writeError)) {
                // Some transports publish a file even though the last Write reported failure.
                // Recover only if the named object reads back byte-for-byte identical.
                for (int attempt = 0; attempt < 3; ++attempt) {
                    abort.sleep(0.15);
                    try {
                        auto found = find(parent, name);
                        if (found && readBack(found->id, abort, &src) == expected) {
                            found->size = expected;
                            found->hasSize = true;
                            return *found;
                        }
                    } catch (const exception_aborted &) {
                        throw;
                    } catch (const std::exception &) {
                    }
                }
            }
            throw;
        }
        CComQIPtr<IPortableDeviceDataStream> data(stream);
        LPWSTR raw = nullptr;
        if (!data)
            throw std::runtime_error(tr("streamId"));
        check(data->GetObjectID(&raw), "Read new MTP object ID");
        auto id = utf(raw);
        CoTaskMemFree(raw);
        data.Release();
        stream.Release();
        try {
            auto written = object(id);
            if (!written.hasSize || written.size != expected) {
                const auto actual = readBack(id, abort, &src);
                if (actual != expected)
                    throw std::runtime_error(message("sizeVerification", {std::to_string(expected), std::to_string(actual)}));
                written.size = actual;
                written.hasSize = true;
            }
            if (CompareStringOrdinal(wide(written.name).c_str(), -1, wide(name).c_str(), -1, TRUE) !=
                CSTR_EQUAL)
                throw std::runtime_error(message("renamedUnexpected", {written.name}));
            return written;
        } catch (...) {
            try {
                discard(id);
            } catch (const std::exception &e) {
                console::print(e.what());
            }
            throw;
        }
    }
};
struct Match {
    Endpoint endpoint;
    S pnp, id;
};
std::vector<Match> enumerate() {
    auto mgr = make<IPortableDeviceManager>(CLSID_PortableDeviceManager);
    DWORD count = 0;
    check(mgr->GetDevices(nullptr, &count), "Count MTP devices");
    if (!count)
        return {};
    std::vector<LPWSTR> ids(count, nullptr);
    check(mgr->GetDevices(ids.data(), &count), "List MTP devices");
    std::vector<Match> out;
    for (DWORD i = 0; i < count; ++i) {
        S pnp = utf(ids[i]);
        CoTaskMemFree(ids[i]);
        try {
            Session s(pnp, false);
            DWORD length = 0;
            mgr->GetDeviceFriendlyName(wide(pnp).c_str(), nullptr, &length);
            std::vector<wchar_t> name(length + 1);
            mgr->GetDeviceFriendlyName(wide(pnp).c_str(), name.data(), &length);
            for (auto &id : s.children("DEVICE")) {
                auto v = s.values(id);
                GUID cat{};
                if (FAILED(v->GetGuidValue(WPD_FUNCTIONAL_OBJECT_CATEGORY, &cat)) ||
                    cat != WPD_FUNCTIONAL_CATEGORY_STORAGE)
                    continue;
                auto stable = stringValue(v, WPD_OBJECT_PERSISTENT_UNIQUE_ID);
                if (stable.empty())
                    continue;
                auto label = stringValue(v, WPD_OBJECT_NAME);
                auto key = "mtp:" + pnp + "|" + stable + "|" + stringValue(v, WPD_STORAGE_SERIAL_NUMBER);
                out.push_back({{key, utf(name.data()) + " / " + label}, pnp, id});
            }
        } catch (const std::exception &e) {
            console::print(e.what());
        }
    }
    return out;
}
class Portable final : public Storage {
    Session session;
    S storage, root;
    std::map<S, S> dirs;
    std::pair<S, S> split(const S &rel, bool create) {
        auto path = fs::path(wide(normalizeRelative(root + "\\" + rel)));
        S id = storage;
        for (auto &part : path.parent_path()) {
            auto name = utf(part.wstring());
            auto key = id + "\n" + name;
            if (auto it = dirs.find(key); it != dirs.end()) {
                id = it->second;
                continue;
            }
            auto child = session.find(id, name);
            if (child && child->type != WPD_CONTENT_TYPE_FOLDER)
                throw std::runtime_error(tr("directoryOccupied"));
            if (!child && !create)
                return {"", {}};
            id = child ? child->id : session.folder(id, name);
            dirs.emplace(key, id);
        }
        return {id, utf(path.filename().wstring())};
    }

  public:
    Portable(const Match &m, const S &path) : session(m.pnp, true), storage(m.id), root(path) {}
    std::optional<Stamp> stat(const S &rel) override {
        auto [parent, name] = split(rel, false);
        if (parent.empty())
            return {};
        auto obj = session.find(parent, name);
        if (!obj)
            return {};
        auto size = session.resourceSize(obj->id);
        if (!size && obj->hasSize && obj->size)
            size = obj->size;
        if (!size || (*size == 0 && obj->hasSize && obj->size != 0))
            size = session.readBack(obj->id, fb2k::noAbort);
        return session.stamp(*obj, *size);
    }
    Stamp put(const S &rel, const fs::path &src, bool replace, abort_callback &abort) override {
        auto [parent, name] = split(rel, true);
        auto old = session.find(parent, name);
        if (old && !replace)
            throw std::runtime_error(tr("fileExists"));
        if (!old) {
            auto file = session.upload(parent, name, src, abort);
            return session.stamp(file, file.size);
        }
        auto tmp = "walkman-" + unique() + utf(fs::path(wide(name)).extension().wstring());
        auto uploaded = session.upload(parent, tmp, src, abort);
        const auto &stage = uploaded.id;
        auto backup = "walkman-backup-" + unique() + ".bak";
        try {
            session.renameObject(old->id, backup);
        } catch (...) {
            session.discard(stage);
            throw;
        }
        try {
            session.renameObject(stage, name);
        } catch (...) {
            session.renameObject(old->id, name);
            session.discard(stage);
            throw;
        }
        session.discard(old->id);
        return session.stamp(session.object(stage), uploaded.size);
    }
    void rename(const S &from, const S &to) override {
        auto [parent, name] = split(from, false);
        auto [dest, newName] = split(to, false);
        if (parent.empty() || parent != dest || session.find(dest, newName))
            throw std::runtime_error(tr("renameConflict"));
        auto obj = session.find(parent, name);
        if (!obj)
            throw std::runtime_error(tr("oldPlaylistMissing"));
        session.renameObject(obj->id, newName);
    }
};
} // namespace
std::vector<Endpoint> portableEndpoints() {
    std::vector<Endpoint> out;
    for (auto &m : enumerate())
        out.push_back(m.endpoint);
    return out;
}
std::unique_ptr<Storage> portableStorage(const Device &d) {
    for (auto &m : enumerate())
        if (m.endpoint.key == d.location)
            return std::make_unique<Portable>(m, d.root);
    throw std::runtime_error(tr("mtpOffline"));
}
} // namespace wm
