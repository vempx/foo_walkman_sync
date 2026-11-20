#include "sync.h"
#include <atlbase.h>
#include <shellapi.h>
#include <cctype>

namespace wm {
const std::vector<CodecMode> &codecModes(const S &codec) {
    static const std::map<S, std::vector<CodecMode>> modes = {
        {"mp3", {{"vbr","可变比特率 (VBR)","--silent -V %v %s %d","VBR 品质 (0 最佳，9 最低)",0,9,2},
                 {"abr","平均比特率 (ABR)","--silent --abr %v %s %d","目标比特率 (8–320 kbps)",8,320,192},
                 {"cbr","恒定比特率 (CBR)","--silent --cbr -b %v %s %d","恒定比特率 (kbps)",8,320,192}}},
        {"opus",{{"vbr","可变比特率 (VBR)","--quiet --vbr --bitrate %v %s %d","目标比特率 (6–510 kbps)",6,510,128},
                 {"cvbr","约束 VBR","--quiet --cvbr --bitrate %v %s %d","目标比特率 (6–510 kbps)",6,510,128},
                 {"cbr","恒定比特率 (CBR)","--quiet --hard-cbr --bitrate %v %s %d","目标比特率 (6–510 kbps)",6,510,128}}},
        {"vorbis",{{"vbr","按品质 (VBR)","-Q -q %v -o %d %s","VBR 品质 (0–10)",0,10,5},
                   {"managed","受控平均比特率","-Q --managed -b %v -o %d %s","目标比特率 (kbps)",8,500,160}}},
        {"nero",{{"vbr","可变比特率 (VBR)","-q %v -if %s -of %d","VBR 品质 (0–100)",0,100,60},
                 {"abr","目标比特率 (ABR)","-br %v -if %s -of %d","目标比特率 (kbps)",8,576,192},
                 {"cbr","恒定比特率 (CBR)","-cbr %v -if %s -of %d","目标比特率 (kbps)",8,576,192}}},
        {"alac",{{"standard","标准","-o %d %s","",0,0,0},
                 {"fast","快速立体声","--fast -o %d %s","",0,0,0}}}
    };
    static const std::vector<CodecMode> none;
    auto it = modes.find(codec);
    return it == modes.end() ? none : it->second;
}
void selectMode(Encoder &f, const S &id) {
    for (const auto &m : codecModes(f.id)) {
        if (m.id != id) continue;
        if (!f.mode.empty()) f.otherValues[f.mode] = f.value;
        f.mode = m.id;
        f.parameter = m.parameter;
        f.arguments = m.arguments;
        f.minimum = m.minimum;
        f.maximum = m.maximum;
        f.value = f.otherValues.contains(id) ? f.otherValues.at(id) : m.initial;
        return;
    }
}
std::vector<Encoder> defaultEncoders() {
    std::vector<Encoder> result = {{"aac", "AAC (Apple)", "qaac64.exe", "", "m4a", -32, {}},
            {"mp3", "MP3 (LAME)", "lame.exe", "", "mp3", 24, {}},
            {"flac", "FLAC", "flac.exe", "--silent -%v -o %d %s", "flac", 0, "压缩级别 (0–8)", 0, 8, 5},
            {"opus", "Opus", "opusenc.exe", "", "opus", 24, {}},
            {"vorbis", "Ogg Vorbis", "oggenc2.exe", "", "ogg", 24, {}},
            {"nero", "AAC (Nero)", "neroAacEnc.exe", "", "m4a", 24, {}},
            {"alac", "Apple Lossless (refalac)", "refalac64.exe", "-o %d %s", "m4a", 0, {}}};
    for (auto &f : result)
        if (!codecModes(f.id).empty()) selectMode(f, codecModes(f.id).front().id);
    return result;
}
const Encoder &Device::output() const {
    for (const auto &f : formats)
        if (f.id == format)
            return f;
    throw std::runtime_error("找不到所选的编码格式");
}
void validateEncoder(const Encoder &f) {
    if (f.name.empty() || f.executable.empty())
        throw std::runtime_error("请输入编码器名称和程序文件名");
    fs::path path = wide(f.executable);
    if (_wcsicmp(path.extension().c_str(), L".exe") || (!path.is_absolute() && path.has_parent_path()))
        throw std::runtime_error("编码器应为 .exe 的文件名或完整路径");
    if (f.extension.empty() || f.extension.size() > 24 ||
        f.extension.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789") !=
            S::npos ||
        !_stricmp(f.extension.c_str(), "m3u8") || !_stricmp(f.extension.c_str(), "m3u"))
        throw std::runtime_error("请填写音频扩展名，例如 opus 或 m4a");
    if (f.custom() && f.arguments.find("%d") == S::npos)
        throw std::runtime_error("参数中需要用 %d 指定输出文件");
    if (f.custom() && f.bits != 16 && f.bits != 24 && f.bits != 32 && f.bits != -32)
        throw std::runtime_error("无效的输入 PCM 格式");
    if (!f.parameter.empty() && (f.value < f.minimum || f.value > f.maximum))
        throw std::runtime_error(f.parameter + "：参数超出范围");
    if (f.id == "mp3" && f.mode == "cbr") {
        static const std::set<int> allowed = {8,16,24,32,40,48,56,64,80,96,112,128,144,160,192,224,256,320};
        if (!allowed.contains(f.value))
            throw std::runtime_error("LAME CBR 可选 kbps：8、16、24、32、40、48、56、64、80、96、112、128、144、160、192、224、256、320");
    }
}
static S arguments(const Device &d) {
    const auto &f = d.output();
    if (f.id == "aac") {
        if (d.aacMode != "tvbr" && d.aacMode != "cvbr" && d.aacMode != "abr" && d.aacMode != "cbr")
            throw std::runtime_error("未知的 AAC 比特率模式");
        return "--" + d.aacMode + " " + std::to_string(d.aacMode == "tvbr" ? d.quality : d.bitrate) +
               " -o %d %s";
    }
    S args = f.arguments;
    if (!f.custom() && !f.parameter.empty()) {
        S v = std::to_string(f.value);
        if (f.id == "nero")
            v = f.mode == "vbr" ? (f.value == 100 ? "1" : "0." + std::to_string(f.value + 100).substr(1))
                                : std::to_string(f.value * 1000);
        args.replace(args.find("%v"), 2, v);
    }
    return args;
}
S encoderSignature(const Device &d) {
    if (d.format == "aac")
        return "aac:" + (d.aacMode == "tvbr" ? std::to_string(d.quality)
                                             : d.aacMode + ":" + std::to_string(d.bitrate));
    const auto &f = d.output();
    return f.id + "|" + f.executable + "|" + f.extension + "|" + std::to_string(f.bits) + "|" +
           arguments(d);
}
static fs::path resolveExe(const S &name) {
    fs::path file = wide(name);
    if (!file.is_absolute()) {
        auto bundled = encoders() / file;
        if (fs::is_regular_file(bundled))
            return bundled;
    } else if (fs::is_regular_file(file))
        return file;
    throw std::runtime_error("找不到编码器 " + name + "。请安装编码器，或在自定义编码器中指定完整路径。");
}
// Quote each argument after expanding placeholders so paths with spaces remain one argv element.
static std::wstring quoteArg(const std::wstring &arg) {
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        out.append(c == L'"' ? 2 * slashes + 1 : slashes, L'\\');
        out += c;
        slashes = 0;
    }
    out.append(2 * slashes, L'\\');
    return out + L'"';
}
void runEncoder(const Device &d, bool extract, const fs::path &wav, const fs::path &dest,
                abort_callback &abort) {
    auto &f = d.output();
    if (!extract)
        validateEncoder(f);
    auto exe = resolveExe(extract ? "flac.exe" : f.executable);
    auto spec = extract ? S("--silent -o %d %s") : arguments(d);
    int count = 0;
    auto raw = CommandLineToArgvW((L"encoder " + wide(spec)).c_str(), &count);
    if (!raw)
        throw std::runtime_error("无法解析编码参数");
    std::unique_ptr<void, decltype(&LocalFree)> freeArgs(raw, LocalFree);
    auto cmd = quoteArg(exe.wstring());
    for (int i = 1; i < count; ++i) {
        std::wstring expanded;
        const std::wstring arg(raw[i]);
        for (size_t j = 0; j < arg.size(); ++j) {
            if (arg[j] == L'%' && j + 1 < arg.size() && (arg[j + 1] == L'd' || arg[j + 1] == L's')) {
                // The child starts in the staging directory. ASCII leaf names also work with legacy
                // encoders.
                expanded += (arg[++j] == L'd' ? dest : wav).filename().wstring();
            } else
                expanded += arg[j];
        }
        cmd += L" " + quoteArg(expanded);
    }
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    auto logFile = dest.parent_path() / L"encoder-output.log";
    ATL::CHandle input(CreateFileW(spec.find("%s") == S::npos ? wav.c_str() : L"NUL", GENERIC_READ,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr));
    ATL::CHandle log(
        CreateFileW(logFile.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, 0, nullptr));
    if (input.m_h == INVALID_HANDLE_VALUE || log.m_h == INVALID_HANDLE_VALUE)
        throw std::runtime_error("无法打开编码器的输入或日志");
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = input;
    si.StartupInfo.hStdOutput = log;
    si.StartupInfo.hStdError = log;
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<BYTE> buffer(size);
    si.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(buffer.data());
    if (!InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &size))
        check(HRESULT_FROM_WIN32(GetLastError()), "Initialize encoder process");
    std::unique_ptr<_PROC_THREAD_ATTRIBUTE_LIST, decltype(&DeleteProcThreadAttributeList)> attrs(
        si.lpAttributeList, DeleteProcThreadAttributeList);
    HANDLE handles[] = {input, log};
    if (!UpdateProcThreadAttribute(attrs.get(), 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles,
                                   sizeof(handles), nullptr, nullptr))
        check(HRESULT_FROM_WIN32(GetLastError()), "Set encoder handles");
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr,
                        dest.parent_path().c_str(), &si.StartupInfo, &pi))
        check(HRESULT_FROM_WIN32(GetLastError()), "Start encoder");
    ATL::CHandle process(pi.hProcess), thread(pi.hThread);
    while (WaitForSingleObject(process, 100) == WAIT_TIMEOUT) {
        if (abort.is_aborting()) {
            TerminateProcess(process, ERROR_CANCELLED);
            WaitForSingleObject(process, INFINITE);
            abort.check();
        }
    }
    DWORD code = 1;
    GetExitCodeProcess(process, &code);
    if (code || !fs::is_regular_file(dest) || !fs::file_size(dest)) {
        log.Close();
        S message = utf(exe.filename().wstring()) + " 未生成有效输出，退出码 " + std::to_string(code);
        std::ifstream file(logFile, std::ios::binary | std::ios::ate);
        if (file) {
            auto n = std::min<std::streamoff>(file.tellg(), 1200);
            if (n > 0) {
                file.seekg(-n, std::ios::end);
                S tail((size_t)n, ' ');
                file.read(tail.data(), n);
                for (char &c : tail)
                    if ((unsigned char)c < 32 && c != '\n' && c != '\t')
                        c = ' ';
                try {
                    wide(tail);
                    message += "\n" + tail;
                } catch (const std::exception &) {
                }
            }
        }
        throw std::runtime_error(message);
    }
}
} // namespace wm
