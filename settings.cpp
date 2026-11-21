#include "sync.h"
#include "resource.h"
#include <foobar2000/SDK/dsp_manager.h>
#include <foobar2000/SDK/coreDarkMode.h>
#include <array>
#include <commdlg.h>
#include <commctrl.h>
#pragma comment(lib, "comdlg32.lib")

namespace wm {
namespace {
S value(HWND w, int id) {
    auto ctrl = GetDlgItem(w, id);
    int n = GetWindowTextLengthW(ctrl);
    std::wstring buf(n + 1, 0);
    buf.resize(GetWindowTextW(ctrl, buf.data(), n + 1));
    return utf(buf);
}
void set(HWND w, int id, const S &s) { SetDlgItemTextW(w, id, wide(s).c_str()); }
LRESULT msg(HWND w, int id, UINT m, WPARAM a = 0, LPARAM b = 0) {
    return SendDlgItemMessageW(w, id, m, a, b);
}
void add(HWND w, int id, const S &s, bool list = false) {
    msg(w, id, list ? LB_ADDSTRING : CB_ADDSTRING, 0, (LPARAM)wide(s).c_str());
}
void enable(HWND w, int id, bool yes) { EnableWindow(GetDlgItem(w, id), yes); }
class DialogLabels {
    std::vector<std::pair<HWND, S>> labels;

    void capture(HWND control) {
        wchar_t text[96]{};
        GetWindowTextW(control, text, _countof(text));
        if (text[0] == L'@') labels.emplace_back(control, utf(text + 1));
    }

  public:
    explicit DialogLabels(HWND window) {
        capture(window);
        for (HWND control = GetWindow(window, GW_CHILD); control;
             control = GetWindow(control, GW_HWNDNEXT))
            capture(control);
        update();
    }
    void update() const {
        for (const auto &[window, key] : labels)
            SetWindowTextW(window, wide(tr(key)).c_str());
    }
};
struct AddDialog {
    std::vector<Endpoint> choices;
    fb2k::CCoreDarkModeHooks dark;
    static INT_PTR CALLBACK proc(HWND w, UINT m, WPARAM a, LPARAM b) {
        if (m == WM_INITDIALOG) {
            auto &self = *(AddDialog *)b;
            self.dark.AddDialogWithControls(w);
            DialogLabels labels(w);
            for (auto &e : self.choices)
                add(w, LOCATION, e.label);
            msg(w, LOCATION, CB_SETCURSEL, 0);
            return TRUE;
        }
        if (m == WM_COMMAND && LOWORD(a) == IDOK) {
            EndDialog(w, 1 + msg(w, LOCATION, CB_GETCURSEL));
            return TRUE;
        }
        if (m == WM_CLOSE || (m == WM_COMMAND && LOWORD(a) == IDCANCEL)) {
            EndDialog(w, 0);
            return TRUE;
        }
        return FALSE;
    }
};
constexpr std::array<const char *, 3> policies = {"copy", "lossless", "all"};
struct CustomDialog {
    Encoder encoder;
    bool existing;
    fb2k::CCoreDarkModeHooks dark;
    static constexpr std::array<int, 4> depths = {16, 24, 32, -32};
    static INT_PTR CALLBACK proc(HWND w, UINT m, WPARAM a, LPARAM b) {
        auto self = reinterpret_cast<CustomDialog *>(GetWindowLongPtrW(w, GWLP_USERDATA));
        try {
            if (m == WM_INITDIALOG) {
                SetWindowLongPtrW(w, GWLP_USERDATA, b);
                self = reinterpret_cast<CustomDialog *>(b);
                self->dark.AddDialogWithControls(w);
                DialogLabels labels(w);
                auto &f = self->encoder;
                set(w, NAME, f.name);
                set(w, PROGRAM, f.executable);
                set(w, EXTENSION, f.extension);
                set(w, ARGUMENTS, f.arguments);
                for (auto text : {tr("int16"), tr("int24"), tr("int32"), tr("float32")})
                    add(w, PRECISION, text);
                msg(w, PRECISION, CB_SETCURSEL,
                    std::find(depths.begin(), depths.end(), f.bits) - depths.begin());
                ShowWindow(GetDlgItem(w, REMOVE_CODEC), self->existing ? SW_SHOW : SW_HIDE);
                return TRUE;
            }
            if (m == WM_CLOSE || (m == WM_COMMAND && LOWORD(a) == IDCANCEL)) {
                EndDialog(w, IDCANCEL);
                return TRUE;
            }
            if (m != WM_COMMAND || !self)
                return FALSE;
            auto &f = self->encoder;
            switch (LOWORD(a)) {
            case BROWSE_EXE: {
                wchar_t path[32768]{};
                auto dir = encoders().wstring();
                OPENFILENAMEW ofn{sizeof(ofn)};
                ofn.hwndOwner = w;
                ofn.lpstrFile = path;
                ofn.nMaxFile = _countof(path);
                auto filter = wide(tr("encoderFilter"));
                constexpr wchar_t pattern[] = L"\0*.exe\0";
                filter.append(pattern, std::size(pattern) - 1);
                ofn.lpstrFilter = filter.c_str();
                ofn.lpstrInitialDir = dir.c_str();
                ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST;
                if (GetOpenFileNameW(&ofn)) {
                    fs::path selected(path);
                    set(w, PROGRAM,
                        utf((_wcsicmp(selected.parent_path().c_str(), dir.c_str()) == 0
                                 ? selected.filename()
                                 : selected)
                                .wstring()));
                }
                return TRUE;
            }
            case IDOK:
                f.name = value(w, NAME);
                f.executable = value(w, PROGRAM);
                f.extension = value(w, EXTENSION);
                if (f.extension.starts_with('.'))
                    f.extension.erase(0, 1);
                f.arguments = value(w, ARGUMENTS);
                f.bits = depths.at(msg(w, PRECISION, CB_GETCURSEL));
                validateEncoder(f);
                EndDialog(w, IDOK);
                return TRUE;
            case REMOVE_CODEC:
                EndDialog(w, REMOVE_CODEC);
                return TRUE;
            }
        } catch (const std::exception &e) {
            error(e);
        }
        return FALSE;
    }
};
struct AudioDialog {
    Device device;
    const std::vector<S> &algorithms;
    fb2k::CCoreDarkModeHooks dark;
    static constexpr std::array<const char *, 4> modes = {"tvbr", "cvbr", "abr", "cbr"};
    static constexpr std::array<unsigned, 5> rates44 = {11025, 22050, 44100, 88200, 176400};
    static constexpr std::array<unsigned, 5> rates48 = {12000, 24000, 48000, 96000, 192000};
    void showValue(HWND w) {
        const auto &f = device.output();
        const bool apple = f.id == "aac";
        const auto &choices = codecModes(f.id);
        msg(w, AACMODE, CB_RESETCONTENT);
        int selectedMode = 0;
        if (apple) {
            for (auto s : {tr("vbr"), tr("cvbr"), tr("abr"), tr("cbr")})
                add(w, AACMODE, s);
            auto it = std::find(modes.begin(), modes.end(), device.aacMode);
            if (it != modes.end()) selectedMode = int(it - modes.begin());
        } else {
            for (size_t i = 0; i < choices.size(); ++i) {
                add(w, AACMODE, tr(choices[i].label));
                if (choices[i].id == f.mode) selectedMode = int(i);
            }
        }
        msg(w, AACMODE, CB_SETCURSEL, selectedMode);
        set(w, MODE_LABEL, f.id == "alac" ? tr("encodeMode") : tr("bitrateMode"));
        for (int id : {AACMODE, MODE_LABEL})
            ShowWindow(GetDlgItem(w, id), apple || !choices.empty() ? SW_SHOW : SW_HIDE);
        for (int id : {QUALITY, QUALITY_LABEL})
            ShowWindow(GetDlgItem(w, id), apple || !f.parameter.empty() ? SW_SHOW : SW_HIDE);
        enable(w, EDIT_CODEC, f.custom());
        if (!apple) {
            set(w, QUALITY_LABEL, tr(f.parameter));
            SetDlgItemInt(w, QUALITY, f.value, FALSE);
            return;
        }
        const bool vbr = device.aacMode == "tvbr";
        set(w, QUALITY_LABEL, vbr ? tr("vbr127") : tr("targetBitrate"));
        SetDlgItemInt(w, QUALITY, vbr ? device.quality : device.bitrate, FALSE);
    }
    void captureValue(HWND w) {
        BOOL ok;
        int v = GetDlgItemInt(w, QUALITY, &ok, FALSE);
        auto &f = selectedFormat();
        if (f.id != "aac") {
            if (!f.parameter.empty())
                f.value = ok ? v : -1;
        } else if (device.aacMode == "tvbr")
            device.quality = ok ? v : -1;
        else
            device.bitrate = ok ? v : -1;
    }
    Encoder &selectedFormat() { return device.formats.at(&device.output() - device.formats.data()); }
    void listFormats(HWND w) {
        msg(w, CODEC, CB_RESETCONTENT);
        for (auto &f : device.formats)
            add(w, CODEC, f.name);
        msg(w, CODEC, CB_SETCURSEL, &device.output() - device.formats.data());
        showValue(w);
        showRates(w);
    }
    void custom(HWND w, bool edit) {
        captureValue(w);
        CustomDialog dialog{edit ? device.output() : Encoder{"custom:" + unique(), "", "", "", "", 24, {}},
                            edit};
        auto result = DialogBoxParamW(core_api::get_my_instance(), MAKEINTRESOURCEW(IDD_CUSTOM), w,
                                      CustomDialog::proc, (LPARAM)&dialog);
        if (result == REMOVE_CODEC && edit) {
            auto index = &device.output() - device.formats.data();
            device.formats.erase(device.formats.begin() + index);
            device.format = "aac";
        } else if (result == IDOK) {
            if (edit)
                selectedFormat() = dialog.encoder;
            else
                device.formats.push_back(dialog.encoder);
            device.format = dialog.encoder.id;
        }
        listFormats(w);
    }
    void init(HWND w) {
        dark.AddDialogWithControls(w);
        DialogLabels labels(w);
        listFormats(w);
        for (auto &name : algorithms)
            add(w, RESAMPLER, name);
        auto algo = std::find(algorithms.begin(), algorithms.end(), device.resampler);
        msg(w, RESAMPLER, CB_SETCURSEL, algo == algorithms.end() ? -1 : algo - algorithms.begin());
    }
    void showRates(HWND w) {
        const bool apple = device.format == "aac";
        auto loadRates = [&](int id, const auto &rates, unsigned current) {
            msg(w, id, CB_RESETCONTENT);
            for (auto rate : rates)
                if (!apple || rate <= 48000)
                    add(w, id, std::to_string(rate) + " Hz");
            msg(w, id, CB_SETCURSEL, std::find(rates.begin(), rates.end(), current) - rates.begin());
        };
        loadRates(RATE44, rates44, apple ? device.appleRate44 : device.rate44);
        loadRates(RATE48, rates48, apple ? device.appleRate48 : device.rate48);
        set(w, DOWNSAMPLE, apple ? tr("downsampleAac") : tr("downsample"));
        CheckDlgButton(w, DOWNSAMPLE, device.usesFamilyResampler() ? BST_CHECKED : BST_UNCHECKED);
        enable(w, DOWNSAMPLE, !apple);
        for (int id : {RESAMPLER, RATE44, RATE48})
            enable(w, id, device.usesFamilyResampler());
    }
    void accept(HWND w) {
        captureValue(w);
        validateEncoder(device.output());
        if (device.format == "aac" && device.aacMode == "tvbr" &&
            (device.quality < 0 || device.quality > 127))
            throw std::runtime_error(tr("qualityRange"));
        if (device.format == "aac" && device.aacMode != "tvbr" &&
            (device.bitrate < 8 || device.bitrate > 320))
            throw std::runtime_error(tr("bitrateRange"));
        auto algo = msg(w, RESAMPLER, CB_GETCURSEL);
        if (device.usesFamilyResampler() && algo < 0)
            throw std::runtime_error(tr("selectResampler"));
        if (algo >= 0)
            device.resampler = algorithms.at(algo);
        if (device.bitrate < 8 || device.bitrate > 320)
            device.bitrate = 256;
        if (device.quality < 0 || device.quality > 127)
            device.quality = 127;
        EndDialog(w, IDOK);
    }
    static INT_PTR CALLBACK proc(HWND w, UINT m, WPARAM a, LPARAM b) {
        auto self = (AudioDialog *)GetWindowLongPtrW(w, GWLP_USERDATA);
        try {
            if (m == WM_INITDIALOG) {
                SetWindowLongPtrW(w, GWLP_USERDATA, b);
                ((AudioDialog *)b)->init(w);
                return TRUE;
            }
            if (m == WM_CLOSE || (m == WM_COMMAND && LOWORD(a) == IDCANCEL)) {
                EndDialog(w, IDCANCEL);
                return TRUE;
            }
            if (m != WM_COMMAND || !self)
                return FALSE;
            switch (LOWORD(a)) {
            case CODEC:
                if (HIWORD(a) != CBN_SELCHANGE)
                    return FALSE;
                self->captureValue(w);
                self->device.format = self->device.formats.at(msg(w, CODEC, CB_GETCURSEL)).id;
                self->showValue(w);
                self->showRates(w);
                return TRUE;
            case ADD_CODEC:
                self->custom(w, false);
                return TRUE;
            case EDIT_CODEC:
                self->custom(w, true);
                return TRUE;
            case IDOK:
                self->accept(w);
                return TRUE;
            case AACMODE:
                if (HIWORD(a) != CBN_SELCHANGE)
                    return FALSE;
                self->captureValue(w);
                if (self->device.format == "aac")
                    self->device.aacMode = modes.at(msg(w, AACMODE, CB_GETCURSEL));
                else
                    selectMode(self->selectedFormat(), codecModes(self->device.format).at(msg(w, AACMODE, CB_GETCURSEL)).id);
                self->showValue(w);
                return TRUE;
            case DOWNSAMPLE:
                if (self->device.format == "aac")
                    return TRUE;
                self->device.downsample = IsDlgButtonChecked(w, DOWNSAMPLE) == BST_CHECKED;
                self->showRates(w);
                return TRUE;
            case RATE44:
            case RATE48:
                if (HIWORD(a) != CBN_SELCHANGE)
                    return FALSE;
                if (LOWORD(a) == RATE44)
                    (self->device.format == "aac" ? self->device.appleRate44 : self->device.rate44) =
                        rates44.at(msg(w, RATE44, CB_GETCURSEL));
                else
                    (self->device.format == "aac" ? self->device.appleRate48 : self->device.rate48) =
                        rates48.at(msg(w, RATE48, CB_GETCURSEL));
                return TRUE;
            case CHAIN: {
                dsp_chain_config_impl chain;
                restoreChain(chain, self->device.chain);
                if (dsp_config_manager::get()->configure_popup(chain, w, tr("dspTitle")))
                    self->device.chain = serializeChain(chain);
                return TRUE;
            }
            }
        } catch (const std::exception &e) {
            error(e);
        }
        return FALSE;
    }
};
class Page : public preferences_page_instance, private playlist_callback_single_impl_base {
    HWND window = nullptr;
    preferences_page_callback::ptr callback;
    fb2k::CCoreDarkModeHooks dark;
    std::vector<Device> all;
    std::vector<S> algorithms, subIds;
    std::map<std::pair<S, S>, bool> edits;
    std::set<S> changed;
    std::optional<DialogLabels> labels;
    int selectedLanguage = language();
    int selected = -1;
    bool rendering = false;
    struct LayoutItem {
        int id;
        RECT bounds;
    };
    std::vector<LayoutItem> layoutItems;
    RECT templateBounds{};
    UINT templateDpi = 96;

    void rememberLayout() {
        GetClientRect(window, &templateBounds);
        templateDpi = GetDpiForWindow(window);
        for (int id : {LANGUAGE_CHOICE, DEVICES, ADD, NAME, ROOTDIR, PATTERN, PREVIEW, PREVIEW_FILE, FORMAT, AUDIO, SUBS,
                       AUTO_SYNC, REPORTS, SYNC_ALL, CLEAR_RECORDS, DELETE_DEVICE}) {
            HWND control = GetDlgItem(window, id);
            RECT bounds{};
            GetWindowRect(control, &bounds);
            MapWindowPoints(nullptr, window, reinterpret_cast<POINT *>(&bounds), 2);
            if (id == DEVICES || id == FORMAT || id == LANGUAGE_CHOICE) {
                RECT dropped{};
                if (SendMessageW(control, CB_GETDROPPEDCONTROLRECT, 0, reinterpret_cast<LPARAM>(&dropped)))
                    bounds.bottom = bounds.top + dropped.bottom - dropped.top;
            }
            layoutItems.push_back({id, bounds});
        }
    }

    void resizeLayout() {
        RECT client{};
        GetClientRect(window, &client);
        const UINT dpi = GetDpiForWindow(window);
        const auto scale = [&](int px) { return MulDiv(px, dpi, templateDpi); };
        const int dx = client.right - scale(templateBounds.right);
        const int dy = client.bottom - scale(templateBounds.bottom);
        for (const auto &[id, bounds] : layoutItems) {
            int x = scale(bounds.left), y = scale(bounds.top);
            int right = scale(bounds.right), bottom = scale(bounds.bottom);
            if (id == ADD || id == DELETE_DEVICE || id == AUDIO || id == REPORTS || id == CLEAR_RECORDS)
                x += dx;
            if (id != AUTO_SYNC && id != SYNC_ALL)
                right += dx;
            if (id == REPORTS || id == AUTO_SYNC || id == SYNC_ALL || id == CLEAR_RECORDS)
                y += dy;
            if (id == REPORTS || id == AUTO_SYNC || id == SUBS || id == SYNC_ALL || id == CLEAR_RECORDS)
                bottom += dy;
            SetWindowPos(GetDlgItem(window, id), nullptr, x, y, std::max(1, right - x),
                         std::max(1, bottom - y), SWP_NOZORDER | SWP_NOACTIVATE);
        }
        sizeColumns();
    }
    void sizeColumns() {
        HWND list = GetDlgItem(window, SUBS);
        RECT area{};
        GetClientRect(list, &area);
        int width = MulDiv(76, GetDpiForWindow(window), 96);
        ListView_SetColumnWidth(list, playlistColumn, std::max(1, int(area.right) - width));
        ListView_SetColumnWidth(list, checkColumn, width);
    }
    bool valid() { return selected >= 0 && selected < (int)all.size(); }
    void touch() {
        if (valid())
            changed.insert(all[selected].id);
        callback->on_state_changed();
    }
    void preview() {
        if (!window || !valid())
            return;
        S directory, filename;
        try {
            auto pm = playlist_manager::get();
            metadb_handle_ptr h;
            titleformat_object::ptr tf;
            auto pat = value(window, PATTERN);
            if (!pm->activeplaylist_get_focus_item_handle(h)) {
                directory = tr("previewSelect");
            } else if (titleformat_compiler::get()->compile(tf, pat.c_str())) {
                pfc::string8 name;
                titleformat_text_filter_impl_filename_chars filter;
                h->format_title(nullptr, name, tf, &filter);
                file_info_impl info;
                h->get_info(info);
                auto ext = utf(fs::path(wide(h->get_path())).extension().wstring());
                auto mode = transferMode(all[selected], info, h->get_subsong_index(), ext);
                auto path = value(window, ROOTDIR) + "\\" + normalizeRelative(name.c_str()) +
                            outputExtension(all[selected], mode, ext);
                auto split = path.rfind('\\') + 1;
                directory = path.substr(0, split);
                filename = path.substr(split);
            }
        } catch (...) {
        }
        set(window, PREVIEW, directory);
        set(window, PREVIEW_FILE, filename);
    }
    void on_item_focus_change(t_size, t_size) override { preview(); }
    void on_playlist_switch() override { preview(); }
    void on_items_added(t_size, metadb_handle_list_cref, const bit_array &) override { preview(); }
    void on_items_removed(const bit_array &, t_size, t_size) override { preview(); }
    void on_items_replaced(
        const bit_array &,
        const pfc::list_base_const_t<playlist_callback::t_on_items_replaced_entry> &) override {
        preview();
    }
    void on_items_modified(const bit_array &) override { preview(); }
    void capture() {
        if (!valid())
            return;
        auto &d = all[selected];
        d.name = value(window, NAME);
        d.root = value(window, ROOTDIR);
        d.pattern = value(window, PATTERN);
        d.policy = policies.at(msg(window, FORMAT, CB_GETCURSEL));
    }
    HWND subList() const { return GetDlgItem(window, SUBS); }
    static constexpr int playlistColumn = 0, checkColumn = 1;
    std::vector<HWND> checks;
    std::unique_ptr<fb2k::CCoreDarkModeHooks> checkTheme;

    void positionChecks() {
        HWND list = subList();
        RECT client{}, header{};
        GetClientRect(list, &client);
        GetWindowRect(ListView_GetHeader(list), &header);
        MapWindowPoints(HWND_DESKTOP, list, reinterpret_cast<POINT *>(&header), 2);
        const int side = MulDiv(13, GetDpiForWindow(list), 96);
        for (size_t i = 0; i < checks.size(); ++i) {
            RECT cell{};
            if (!ListView_GetSubItemRect(list, int(i), checkColumn, LVIR_BOUNDS, &cell)) continue;
            int x = (cell.left + cell.right - side) / 2;
            int y = (cell.top + cell.bottom - side) / 2;
            HWND box = checks[i];
            if (y < header.bottom || y >= client.bottom || x + side <= 0 || x >= client.right ||
                cell.right - cell.left < side) {
                if (GetWindowLongPtrW(box, GWL_STYLE) & WS_VISIBLE) ShowWindow(box, SW_HIDE);
                continue;
            }
            RECT old{};
            GetWindowRect(box, &old);
            MapWindowPoints(HWND_DESKTOP, list, reinterpret_cast<POINT *>(&old), 2);
            if (old.left != x || old.top != y || old.right - old.left != side ||
                old.bottom - old.top != side || !(GetWindowLongPtrW(box, GWL_STYLE) & WS_VISIBLE))
                SetWindowPos(box, nullptr, x, y, side, side, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }
    }
    void saveCheck(int row) {
        edits[{all.at(selected).id, subIds.at(row)}] =
            SendMessageW(checks.at(row), BM_GETCHECK, 0, 0) == BST_CHECKED;
        callback->on_state_changed();
    }
    static LRESULT CALLBACK listProc(HWND list, UINT m, WPARAM a, LPARAM b, UINT_PTR id, DWORD_PTR data) {
        auto self = reinterpret_cast<Page *>(data);
        if (m == WM_PAINT || m == WM_PRINT || m == WM_PRINTCLIENT) self->positionChecks();
        if (m == WM_COMMAND && HIWORD(a) == BN_CLICKED && b && !self->rendering) {
            int row = GetDlgCtrlID(reinterpret_cast<HWND>(b)) - 1;
            if (row >= 0 && size_t(row) < self->checks.size() && self->checks[row] == reinterpret_cast<HWND>(b)) {
                ListView_SetItemState(list, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                SetFocus(list);
                self->saveCheck(row);
                return 0;
            }
        }
        if (m == WM_NCDESTROY) RemoveWindowSubclass(list, listProc, id);
        return DefSubclassProc(list, m, a, b);
    }
    void manageDevice(bool remove) {
        auto &d = all.at(selected);
        if (transferring(d)) throw std::runtime_error(tr("waitTransfer"));
        S prompt = message(remove ? "removeConfirm" : "clearConfirm", {d.name});
        prompt += "\n\n";
        prompt += tr("keepFiles");
        if (MessageBoxW(window,wide(prompt).c_str(),L"Walkman Sync", MB_YESNO | MB_DEFBUTTON2 | MB_ICONQUESTION) != IDYES)
            return;
        clearDeviceData(d,remove);
        if (remove) {
            changed.erase(d.id);
            std::erase_if(edits,[&](const auto &entry){ return entry.first.first == d.id; });
            all.erase(all.begin()+selected);
            selected = std::min(selected, int(all.size())-1);
            draw();
            callback->on_state_changed();
        }
    }
    void draw() {
        rendering = true;
        labels->update();
        msg(window, FORMAT, CB_RESETCONTENT);
        for (auto key : {"copyOriginal", "convertLossless", "convertAll"})
            add(window, FORMAT, tr(key));
        auto playlistLabel = wide(tr("playlistName"));
        auto autoLabel = wide(tr("autoUpdate"));
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT;
        col.pszText = playlistLabel.data();
        ListView_SetColumn(subList(), playlistColumn, &col);
        col.pszText = autoLabel.data();
        ListView_SetColumn(subList(), checkColumn, &col);
        msg(window, DEVICES, CB_RESETCONTENT);
        for (auto &d : all)
            add(window, DEVICES, d.name + " · " + tr(online(d) ? "online" : "offline"));
        msg(window, DEVICES, CB_SETCURSEL, selected);
        for (int id : {NAME, ROOTDIR, PATTERN, USE_PATTERN, FORMAT, AUDIO, REPORTS, SUBS, AUTO_SYNC, DELETE_DEVICE, CLEAR_RECORDS, SYNC_ALL})
            enable(window, id, valid());
        checkTheme.reset();
        for (HWND box : checks) DestroyWindow(box);
        checks.clear();
        checkTheme = std::make_unique<fb2k::CCoreDarkModeHooks>();
        ListView_DeleteAllItems(subList());
        CheckDlgButton(window, AUTO_SYNC, valid() && all[selected].autoSync ? BST_CHECKED : BST_UNCHECKED);
        subIds.clear();
        if (valid()) {
            auto &d = all[selected];
            set(window, NAME, d.name);
            set(window, ROOTDIR, d.root);
            set(window, PATTERN, d.pattern);
            auto policy = std::find(policies.begin(), policies.end(), d.policy);
            msg(window, FORMAT, CB_SETCURSEL, policy == policies.end() ? 0 : policy - policies.begin());
            auto pm = playlist_manager_v5::get();
            std::map<S, S> subs;
            for (auto &r : Db().query("SELECT list,automatic FROM subscription WHERE device=?", {d.id}))
                subs.emplace(r[0], r[1]);
            for (size_t n = 0; n < pm->get_playlist_count(); ++n) {
                auto key = guid(pm->playlist_get_guid(n));
                if (!subs.contains(key))
                    continue;
                pfc::string8 name;
                pm->playlist_get_name(n, name);
                subIds.push_back(key);
                bool automatic = edits.contains({d.id, key}) ? edits.at({d.id, key}) : subs[key] == "1";
                LVITEMW item{};
                item.mask = LVIF_TEXT;
                item.iItem = int(subIds.size()) - 1;
                auto label = wide(name.c_str());
                item.pszText = label.data();
                int row = ListView_InsertItem(subList(), &item);
                auto box = CreateWindowExW(0, L"Button", label.c_str(), WS_CHILD | BS_AUTOCHECKBOX,
                    0, 0, 0, 0, subList(), reinterpret_cast<HMENU>(INT_PTR(row + 1)),
                    core_api::get_my_instance(), nullptr);
                if (!box) throw std::runtime_error(tr("checkboxError"));
                checks.push_back(box);
                checkTheme->AddCtrlAuto(box);
                SendMessageW(box, BM_SETCHECK, automatic ? BST_CHECKED : BST_UNCHECKED, 0);
            }
            sizeColumns();
            positionChecks();
            preview();
        } else {
            for (int id : {NAME, ROOTDIR, PATTERN, PREVIEW, PREVIEW_FILE}) set(window, id, "");
            msg(window, FORMAT, CB_SETCURSEL, -1);
        }
        rendering = false;
    }
    void command(int id, int notification) {
        if (rendering)
            return;
        if (id == LANGUAGE_CHOICE && notification == CBN_SELCHANGE) {
            auto choice = msg(window, LANGUAGE_CHOICE, CB_GETCURSEL);
            if (choice >= 0 && size_t(choice) < std::size(languageNames)) {
                selectedLanguage = static_cast<int>(choice);
                callback->on_state_changed();
            }
            return;
        }
        if (id == ADD) {
            capture();
            AddDialog dialog{endpoints()};
            auto pos = DialogBoxParamW(core_api::get_my_instance(), MAKEINTRESOURCEW(IDD_ADD), window,
                                       AddDialog::proc, (LPARAM)&dialog);
            if (pos <= 0 || pos > (int)dialog.choices.size())
                return;
            auto endpoint = dialog.choices[pos - 1];
            for (size_t n = 0; n < all.size(); ++n)
                if (all[n].location == endpoint.key) {
                    selected = (int)n;
                    draw();
                    return;
                }
            S algo = algorithms.empty() ? "" : algorithms.front();
            for (auto &name : algorithms)
                if (name.find("SoX") != S::npos || name.find("sox") != S::npos) {
                    algo = name;
                    break;
                }
            S label = endpoint.label.substr(0, endpoint.label.find(" / "));
            if (label.find(":\\") != S::npos)
                label = "Walkman";
            all.push_back({unique(),
                           label,
                           endpoint.key,
                           endpoint.key.starts_with("mtp:") ? "Music" : "MUSIC",
                           defaultPattern,
                           "copy",
                           algo,
                           127,
                           {}});
            selected = (int)all.size() - 1;
            touch();
            draw();
        } else if (id == DEVICES && notification == CBN_SELCHANGE) {
            int n = (int)msg(window, DEVICES, CB_GETCURSEL);
            capture();
            selected = n;
            draw();
        } else if (id == AUTO_SYNC && valid()) {
            all[selected].autoSync = IsDlgButtonChecked(window,AUTO_SYNC) == BST_CHECKED;
            touch();
        } else if (id == SYNC_ALL && valid()) {
            // Use the committed device configuration, just like the File menu.
            for (const auto &d : devices()) {
                if (d.id != all[selected].id) continue;
                if (!online(d)) throw std::runtime_error(tr("connectFirst"));
                start(d);
                return;
            }
            throw std::runtime_error(tr("applyDeviceFirst"));
        } else if ((id == DELETE_DEVICE || id == CLEAR_RECORDS) && valid()) {
            manageDevice(id == DELETE_DEVICE);
        } else if (id == USE_PATTERN && valid()) {
            set(window, PATTERN, defaultPattern);
        } else if (id == AUDIO && valid()) {
            capture();
            AudioDialog dialog{all[selected], algorithms};
            if (DialogBoxParamW(core_api::get_my_instance(), MAKEINTRESOURCEW(IDD_AUDIO), window,
                                AudioDialog::proc, (LPARAM)&dialog) == IDOK) {
                all[selected] = std::move(dialog.device);
                touch();
                preview();
            }
        } else if (id == REPORTS && valid()) {
            S history;
            for (auto &row : Db().query(
                     "SELECT finished,summary FROM report WHERE device=? ORDER BY rowid DESC LIMIT 20",
                     {all[selected].id}))
                history += row[0] + "\n" + row[1] + "\n\n";
            popup_message::g_show(history.empty() ? tr("noHistory") : history.c_str(),
                                  (S("Walkman Sync · ") + tr("history")).c_str());
        } else if (notification == EN_CHANGE && (id == NAME || id == ROOTDIR || id == PATTERN)) {
            touch();
            preview();
        } else if (notification == CBN_SELCHANGE && id == FORMAT) {
            capture();
            touch();
            draw();
        }
    }
    static INT_PTR CALLBACK proc(HWND w, UINT m, WPARAM a, LPARAM b) {
        auto self = (Page *)GetWindowLongPtrW(w, GWLP_USERDATA);
        try {
            if (m == WM_INITDIALOG) {
                self = (Page *)b;
                SetWindowLongPtrW(w, GWLP_USERDATA, b);
                self->window = w;
                self->dark.AddDialogWithControls(w);
                auto host = GetAncestor(w, GA_ROOT);
                auto font = SendMessageW(host, WM_GETFONT, 0, 0);
                if (font)
                    for (HWND c = GetWindow(w, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT))
                        SendMessageW(c, WM_SETFONT, font, FALSE);
                self->labels.emplace(w);
                for (auto name : languageNames)
                    add(w, LANGUAGE_CHOICE, name);
                msg(w, LANGUAGE_CHOICE, CB_SETCURSEL, self->selectedLanguage);
                HWND list = self->subList();
                SetWindowLongPtrW(list, GWL_STYLE, GetWindowLongPtrW(list, GWL_STYLE) | WS_CLIPCHILDREN);
                SetWindowSubclass(list, listProc, 1, reinterpret_cast<DWORD_PTR>(self));
                ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
                LVCOLUMNW col{};
                col.mask = LVCF_TEXT | LVCF_WIDTH;
                col.cx = 120;
                col.pszText = const_cast<wchar_t *>(L"");
                ListView_InsertColumn(list, playlistColumn, &col);
                col.mask |= LVCF_FMT;
                col.fmt = LVCFMT_CENTER;
                ListView_InsertColumn(list, checkColumn, &col);
                self->sizeColumns();
                self->rememberLayout();
                self->draw();
                return TRUE;
            }
            if ((m == WM_SIZE || m == WM_DPICHANGED_AFTERPARENT) && self && self->window) {
                self->resizeLayout();
                return FALSE;
            }
            if (m == WM_NOTIFY && self) {
                auto header = reinterpret_cast<NMHDR *>(b);
                if (header->idFrom == SUBS && header->code == LVN_KEYDOWN &&
                    reinterpret_cast<NMLVKEYDOWN *>(b)->wVKey == VK_SPACE) {
                    int row = ListView_GetNextItem(self->subList(), -1, LVNI_FOCUSED);
                    if (row >= 0 && size_t(row) < self->checks.size())
                        SendMessageW(self->checks[row], BM_CLICK, 0, 0);
                }
            }
            if (m == WM_COMMAND && self) {
                self->command(LOWORD(a), HIWORD(a));
                return TRUE;
            }
            if (m == WM_NCDESTROY && self)
                self->window = nullptr;
        } catch (const std::exception &e) {
            error(e);
        }
        return FALSE;
    }

  public:
    Page(HWND parent, preferences_page_callback::ptr cb)
        : callback(cb), all(devices()), algorithms(resamplers()) {
        selected = all.empty() ? -1 : 0;
        if (!CreateDialogParamW(core_api::get_my_instance(), MAKEINTRESOURCEW(IDD_PREF), parent, proc,
                                (LPARAM)this))
            throw std::runtime_error(tr("openPrefs"));
        set_callback_flags(flag_on_item_focus_change | flag_on_playlist_switch | flag_on_items_added |
                           flag_on_items_removed | flag_on_items_replaced | flag_on_items_modified);
    }
    HWND get_wnd() override { return window; }
    t_uint32 get_state() override {
        return preferences_state::dark_mode_supported |
               (changed.empty() && edits.empty() && selectedLanguage == language() ? 0 : preferences_state::changed);
    }
    void reset() override {}
    void apply() override {
        try {
            capture();
            std::set<S> names;
            for (auto &d : all) {
                if (d.name.empty() || d.name.find_first_of("\r\n\t") != S::npos ||
                    !names.insert(d.name).second)
                    throw std::runtime_error(tr("uniqueName"));
                if (d.root != normalizeRelative(d.root))
                    throw std::runtime_error(tr("badRoot"));
                titleformat_object::ptr fmt;
                if (d.pattern.empty() || !titleformat_compiler::get()->compile(fmt, d.pattern.c_str()))
                    throw std::runtime_error(tr("invalidPattern"));
                if (d.quality < 0 || d.quality > 127)
                    throw std::runtime_error(tr("qualityRange"));
            }
            for (auto &d : all)
                if (changed.contains(d.id))
                    save(d);
            Db db;
            for (auto &[key, on] : edits)
                db.query("UPDATE subscription SET automatic=? WHERE device=? AND list=?",
                         {on ? "1" : "0", key.first, key.second});
            changed.clear();
            edits.clear();
            if (selectedLanguage != language()) setLanguage(selectedLanguage);
            draw();
            callback->on_state_changed();
            poll();
        } catch (const std::exception &e) {
            error(e);
        }
    }
};
class Preferences : public preferences_page_v3 {
  public:
    const char *get_name() override { return "Walkman Sync"; }
    GUID get_guid() override { return prefsId; }
    GUID get_parent_guid() override { return guid_tools; }
    preferences_page_instance::ptr instantiate(HWND parent, preferences_page_callback::ptr cb) override {
        return fb2k::service_new<Page>(parent, cb);
    }
};
preferences_page_factory_t<Preferences> factory;
struct Conflicts {
    const std::vector<S> &paths;
    std::set<S> selection;
    fb2k::CCoreDarkModeHooks dark;
    static INT_PTR CALLBACK proc(HWND w, UINT m, WPARAM a, LPARAM b) {
        auto self = (Conflicts *)GetWindowLongPtrW(w, GWLP_USERDATA);
        if (m == WM_INITDIALOG) {
            self = (Conflicts *)b;
            self->dark.AddDialogWithControls(w);
            DialogLabels labels(w);
            SetWindowLongPtrW(w, GWLP_USERDATA, b);
            for (auto &p : self->paths)
                add(w, FILES, p, true);
            msg(w, FILES, LB_SETHORIZONTALEXTENT, 2000);
            return TRUE;
        }
        if (m == WM_COMMAND && LOWORD(a) == SELECT_ALL) {
            msg(w, FILES, LB_SETSEL, TRUE, -1);
            return TRUE;
        }
        if (m == WM_COMMAND && LOWORD(a) == IDOK) {
            for (size_t i = 0; i < self->paths.size(); ++i)
                if (msg(w, FILES, LB_GETSEL, i) > 0)
                    self->selection.insert(self->paths[i]);
            EndDialog(w, 1);
            return TRUE;
        }
        if (m == WM_CLOSE || (m == WM_COMMAND && LOWORD(a) == IDCANCEL)) {
            EndDialog(w, 0);
            return TRUE;
        }
        return FALSE;
    }
};
} // namespace
std::set<S> chooseConflicts(const std::vector<S> &paths) {
    Conflicts dialog{paths};
    auto result = DialogBoxParamW(core_api::get_my_instance(), MAKEINTRESOURCEW(IDD_CONFLICT),
                                  core_api::get_main_window(), Conflicts::proc, (LPARAM)&dialog);
    if (result < 0)
        throw std::runtime_error(tr("conflictOpen"));
    return dialog.selection;
}
} // namespace wm
