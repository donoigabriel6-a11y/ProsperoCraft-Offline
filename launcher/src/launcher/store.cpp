// Launcher data: see store.hpp.
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cctype>
#include "launcher/store.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace hui::launcher
{

namespace
{

std::string trim(const std::string &s)
{
    std::size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r'))
        ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r'))
        --b;
    return s.substr(a, b - a);
}

bool read_file(const std::string &path, std::string &out)
{
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f)
        return false;
    char buffer[4096];
    std::size_t n;
    out.clear();
    while ((n = std::fread(buffer, 1, sizeof buffer, f)) > 0)
        out.append(buffer, n);
    std::fclose(f);
    return true;
}

bool is_dir(const std::string &path)
{
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool is_file(const std::string &path)
{
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

void make_dirs(const std::string &path)
{
    for (std::size_t i = 1; i <= path.size(); ++i)
        if (i == path.size() || path[i] == '/')
            ::mkdir(path.substr(0, i).c_str(), 0777);
}

std::vector<std::string> list_dir(const std::string &path)
{
    std::vector<std::string> names;
    if (DIR *d = ::opendir(path.c_str()))
    {
        while (const dirent *e = ::readdir(d))
        {
            const std::string n = e->d_name;
            if (n != "." && n != "..")
                names.push_back(n);
        }
        ::closedir(d);
    }
    std::sort(names.begin(), names.end());
    return names;
}

long long tree_size(const std::string &path, int &budget)
{
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0)
        return 0;
    if (!S_ISDIR(st.st_mode))
        return static_cast<long long>(st.st_size);
    long long total = 0;
    for (const std::string &n : list_dir(path))
    {
        if (--budget < 0)
            break;
        total += tree_size(path + "/" + n, budget);
    }
    return total;
}

bool ends_with(const std::string &s, const char *suffix)
{
    const std::size_t n = std::char_traits<char>::length(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

bool write_file_atomic(const std::string &path, const std::string &text)
{
    const std::string tmp = path + ".tmp";
    FILE *f = std::fopen(tmp.c_str(), "wb");
    if (!f)
        return false;
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    std::fclose(f);
    if (!ok || std::rename(tmp.c_str(), path.c_str()) != 0)
    {
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

// Ids are folder names: the same cleaning as Launcher.clean() on the Java side.
std::string clean(const std::string &id)
{
    std::string s;
    for (char c : id)
        s += (std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '-') ? c : '_';
    return s.empty() || s[0] == '.' ? "default" : s;
}

} // namespace

std::string loader_kind(const std::string &id)
{
    if (id.rfind("fabric-loader-", 0) == 0)
        return "fabric";
    if (id.rfind("quilt-loader-", 0) == 0)
        return "quilt";
    if (id.rfind("neoforge-", 0) == 0)
        return "neoforge";
    if (id.find("-forge-") != std::string::npos)
        return "forge";
    return std::string();
}

std::string game_version(const std::string &id)
{
    const std::string k = loader_kind(id);
    if (k == "fabric" || k == "quilt")
    {
        const std::size_t dash = id.rfind('-');
        return dash == std::string::npos ? id : id.substr(dash + 1);
    }
    if (k == "forge")
        return id.substr(0, id.find("-forge-")); // 1.20.1-forge-47.2.0
    if (k == "neoforge") // neoforge-21.1.172 -> 1.21.1, neoforge-21.0.5 -> 1.21, neoforge-20.4.237 -> 1.20.4
    {
        const std::string v = id.substr(9);
        const std::size_t d1 = v.find('.'), d2 = d1 == std::string::npos ? d1 : v.find('.', d1 + 1);
        if (d1 == std::string::npos || d2 == std::string::npos)
            return id;
        const std::string minor = v.substr(d1 + 1, d2 - d1 - 1);
        return "1." + v.substr(0, d1) + (minor == "0" ? std::string() : "." + minor);
    }
    return id;
}

std::string url_encode(const std::string &s)
{
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (const char ch : s)
    {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (std::isalnum(c) || c == '.' || c == '_' || c == '-')
            out += ch;
        else
        {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

std::string size_text(long long bytes)
{
    char b[32];
    if (bytes >= 1024LL * 1024 * 1024)
        std::snprintf(b, sizeof b, "%.1f GB", static_cast<double>(bytes) / (1024.0 * 1024 * 1024));
    else if (bytes >= 1024LL * 1024)
        std::snprintf(b, sizeof b, "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024));
    else if (bytes >= 1024)
        std::snprintf(b, sizeof b, "%.0f KB", static_cast<double>(bytes) / 1024.0);
    else
        std::snprintf(b, sizeof b, "%lld B", bytes);
    return b;
}

std::map<std::string, std::string> parse_properties(const std::string &text)
{
    std::map<std::string, std::string> out;
    std::size_t at = 0;
    while (at <= text.size())
    {
        std::size_t eol = text.find('\n', at);
        if (eol == std::string::npos)
            eol = text.size();
        const std::string line = trim(text.substr(at, eol - at));
        at = eol + 1;
        if (line.empty() || line[0] == '#' || line[0] == '!')
            continue;
        const std::size_t eq = line.find_first_of("=:");
        if (eq == std::string::npos)
            continue;
        out[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
    return out;
}

std::string write_properties(const std::map<std::string, std::string> &props)
{
    std::string out;
    for (const auto &[key, value] : props) // std::map: sorted, like the Java side
    {
        std::string v = value;
        std::replace(v.begin(), v.end(), '\n', ' ');
        std::replace(v.begin(), v.end(), '\r', ' ');
        out += key + "=" + v + "\n";
    }
    return out;
}

std::string Instance::get(const std::string &key, const std::string &fallback) const
{
    const auto it = props.find(key);
    if (it == props.end())
        return fallback;
    const std::string v = trim(it->second);
    return v.empty() ? fallback : v;
}

int Instance::geti(const std::string &key, int fallback) const
{
    const std::string v = get(key);
    if (v.empty())
        return fallback;
    char *end = nullptr;
    const long n = std::strtol(v.c_str(), &end, 10);
    return end && *end == '\0' ? static_cast<int>(n) : fallback;
}

void Instance::set(const std::string &key, const std::string &value)
{
    props[key] = value;
}

void Instance::seti(const std::string &key, int value)
{
    props[key] = std::to_string(value);
}

Store Store::load(const std::string &root)
{
    Store s;
    s.root = root;
    std::string text;
    if (read_file(root + "/launcher.cfg", text))
    {
        const auto cfg = parse_properties(text);
        if (const auto it = cfg.find("instance"); it != cfg.end())
            s.current = clean(it->second);
        if (const auto it = cfg.find("account"); it != cfg.end() && !trim(it->second).empty())
            s.account = trim(it->second);
        if (const auto it = cfg.find("username"); it != cfg.end() && !trim(it->second).empty())
            s.username = trim(it->second);
        if (const auto it = cfg.find("offlineProfiles"); it != cfg.end() && !trim(it->second).empty())
        {
            std::string profiles = trim(it->second);
            std::size_t start = 0;
            while (start < profiles.size())
            {
                std::size_t end = profiles.find(',', start);
                if (end == std::string::npos) end = profiles.size();
                std::string name = profiles.substr(start, end - start);
                if (!name.empty())
                    s.offlineProfiles.push_back(name);
                start = end + 1;
            }
        }
    }
    if (s.current.empty())
        s.current = "default";

    std::vector<std::string> ids{"default"};
    for (const std::string &n : list_dir(root + "/instances"))
        if (n != "default" && is_dir(root + "/instances/" + n))
            ids.push_back(n);
    for (const std::string &id : ids)
    {
        Instance inst;
        inst.id = id;
        if (read_file(root + "/instances/" + id + "/instance.cfg", text))
            inst.props = parse_properties(text);
        s.instances.push_back(std::move(inst));
    }

    for (const std::string &n : list_dir(root + "/versions"))
        if (is_file(root + "/versions/" + n + "/" + n + ".jar") ||
            is_file(root + "/versions/" + n + "/" + n + ".installed"))
            s.versions.push_back(n);
    if (read_file(root + "/versions/selected.txt", text))
        s.selected = trim(text);
    return s;
}

Store Store::demo()
{
    Store s;
    s.root = "";
    s.current = "default";
    s.account = "offline";
    s.versions = {"1.20.1", "1.21.1", "fabric-loader-0.19.5-1.20.1", "fabric-loader-0.19.5-1.21.1"};
    auto add = [&s](const char *id, const char *name, const char *version, const char *loader, int xmx,
                    int cpus, const char *gc) {
        Instance i;
        i.id = id;
        i.set("name", name);
        i.set("version", version);
        if (*loader)
            i.set("loader", loader);
        i.seti("xmx", xmx);
        i.seti("cpus", cpus);
        i.seti("jitThreads", 1);
        i.set("gc", gc);
        s.instances.push_back(i);
    };
    add("default", "Vanilla 1.20.1", "1.20.1", "", 2560, 4, "parallel");
    add("fabric121", "Fabric 1.21.1", "fabric-loader-0.19.5-1.21.1", "fabric", 3072, 4, "parallel");
    add("shaders120", "Shaders 1.20.1", "fabric-loader-0.19.5-1.20.1", "fabric", 4096, 6, "parallel");
    add("survival", "Survival world", "1.21.1", "", 2048, 4, "serial");
    s.selected = "1.20.1";
    const auto item = [](const char *name, long long size, bool disabled = false, bool aside = false) {
        Item i;
        i.name = name;
        i.size = size;
        i.disabled = disabled;
        i.aside = aside;
        return i;
    };
    s.demo_items_["mods"] = {item("fabric-api-0.92.2+1.20.1.jar", 2200000), item("ferritecore-6.0.0-fabric.jar", 140000),
                             item("iris-fabric-1.6.17+mc1.20.1.jar", 1800000), item("lithium-fabric-mc1.20.1-0.11.2.jar", 640000),
                             item("modmenu-7.2.2.jar.disabled", 590000, true), item("sodium-fabric-0.5.8+mc1.20.1.jar", 1400000)};
    s.demo_items_["mods-disabled"] = {item("sodium-fabric-0.8.13+mc1.21.1.jar", 1700000, false, true)};
    s.demo_items_["resourcepacks"] = {item("Faithful 32x - 1.20.1.zip", 24800000), item("Fresh Animations v1.9.zip", 11300000)};
    s.demo_items_["shaderpacks"] = {item("BSL_v8.2.zip", 2100000), item("ComplementaryReimagined_r5.2.2.zip", 1300000)};
    return s;
}

int Store::find(const std::string &id) const
{
    for (std::size_t i = 0; i < instances.size(); ++i)
        if (instances[i].id == id)
            return static_cast<int>(i);
    return -1;
}

std::string Store::game_dir(const Instance &instance) const
{
    const std::string g = instance.get("gameDir");
    if (!g.empty())
        return g;
    return instance.id == "default" ? root : root + "/instances/" + instance.id + "/game";
}

int Store::mod_count(const Instance &instance) const
{
    if (root.empty())
        return instance.loader().empty() ? 0 : (instance.id == "shaders120" ? 6 : 4);
    int n = 0;
    for (const std::string &f : list_dir(game_dir(instance) + "/mods"))
        if (f.size() > 4 && f.compare(f.size() - 4, 4, ".jar") == 0)
            ++n;
    return n;
}

bool Store::save(const Instance &instance) const
{
    if (root.empty())
        return true; // preview data
    const std::string dir = root + "/instances/" + clean(instance.id);
    make_dirs(dir);
    return write_file_atomic(dir + "/instance.cfg", write_properties(instance.props));
}

bool Store::set_config(const std::string &key, const std::string &value)
{
    if (root.empty())
        return true;
    std::string text;
    std::map<std::string, std::string> cfg;
    if (read_file(root + "/launcher.cfg", text))
        cfg = parse_properties(text);
    if (value.empty())
        cfg.erase(key);
    else
        cfg[key] = value;
    make_dirs(root);
    return write_file_atomic(root + "/launcher.cfg", write_properties(cfg));
}

std::string Store::config(const std::string &key) const
{
    std::string text;
    if (root.empty() || !read_file(root + "/launcher.cfg", text))
        return "";
    const auto cfg = parse_properties(text);
    const auto it = cfg.find(key);
    return it == cfg.end() ? "" : it->second;
}

bool Store::select(const std::string &id)
{
    current = clean(id);
    return set_config("instance", current);
}

bool Store::set_selected(const std::string &id)
{
    selected = id;
    if (root.empty())
        return true;
    make_dirs(root + "/versions");
    return write_file_atomic(root + "/versions/selected.txt", id.empty() ? std::string() : id + "\n");
}

void Store::forget(const std::string &id)
{
    if (id == "default")
        return;
    const int at = find(id);
    if (at >= 0)
        instances.erase(instances.begin() + at);
    if (current == id)
        select("default");
}

std::string Store::effective_version(const Instance &instance) const
{
    const std::string v = instance.version();
    return v.empty() && instance.id == "default" ? selected : v;
}

std::vector<std::string> Store::version_users(const std::string &id) const
{
    std::vector<std::string> users;
    for (const Instance &in : instances)
        if (effective_version(in) == id)
            users.push_back(in.name());
    for (const std::string &v : versions)
        if (v != id && v != game_version(v) && game_version(v) == id)
            users.push_back(v);
    return users;
}

long long Store::version_size(const std::string &id) const
{
    if (root.empty())
        return id.rfind("fabric-loader-", 0) == 0 ? 24000 : 27500000;
    int budget = 2000;
    return tree_size(root + "/versions/" + id, budget);
}

std::vector<Item> Store::list_items(const Instance &instance, const std::string &folder) const
{
    if (root.empty())
    {
        const auto it = demo_items_.find(folder);
        return it == demo_items_.end() ? std::vector<Item>{} : it->second;
    }
    const std::string dir = game_dir(instance) + "/" + folder;
    std::vector<Item> out;
    int budget = 20000;
    for (const std::string &n : list_dir(dir))
    {
        if (n.empty() || n[0] == '.')
            continue;
        Item item;
        item.name = n;
        if (folder == "mods" || folder == "mods-disabled")
        {
            item.disabled = ends_with(n, ".jar.disabled");
            if (!ends_with(n, ".jar") && !item.disabled)
                continue;
            item.aside = folder == "mods-disabled";
        }
        else if (!is_dir(dir + "/" + n) && !ends_with(n, ".zip"))
            continue; // settings files such as <pack>.zip.txt
        item.size = tree_size(dir + "/" + n, budget);
        out.push_back(std::move(item));
    }
    return out;
}

void Store::demo_apply(const std::string &command)
{
    // "remove <folder> <name>" or "toggle mods <name>", names percent-encoded like url_encode().
    const auto decode = [](const std::string &e) {
        std::string out;
        for (std::size_t i = 0; i < e.size(); ++i)
            if (e[i] == '%' && i + 2 < e.size() && std::isxdigit(static_cast<unsigned char>(e[i + 1])) &&
                std::isxdigit(static_cast<unsigned char>(e[i + 2])))
            {
                out += static_cast<char>(std::strtol(e.substr(i + 1, 2).c_str(), nullptr, 16));
                i += 2;
            }
            else
                out += e[i];
        return out;
    };
    std::vector<std::string> w;
    for (std::size_t at = 0; at < command.size();)
    {
        const std::size_t sp = command.find(' ', at);
        w.push_back(command.substr(at, sp == std::string::npos ? std::string::npos : sp - at));
        at = sp == std::string::npos ? command.size() : sp + 1;
    }
    if (w.size() < 3)
        return;
    if (w[0] == "remove" && w[1] == "version")
    {
        std::vector<std::string> gone{w[2]};
        for (const std::string &v : versions)
            if (v != w[2] && game_version(v) == w[2])
                gone.push_back(v);
        for (const std::string &g : gone)
        {
            versions.erase(std::remove(versions.begin(), versions.end(), g), versions.end());
            for (Instance &in : instances)
                if (in.version() == g)
                {
                    in.props.erase("version");
                    in.props.erase("loader");
                }
            if (selected == g)
                selected.clear();
        }
        return;
    }
    std::vector<Item> &list = demo_items_[w[1]];
    const std::string name = decode(w[2]);
    for (std::size_t i = 0; i < list.size(); ++i)
    {
        if (list[i].name != name)
            continue;
        if (w[0] == "remove")
            list.erase(list.begin() + static_cast<long>(i));
        else if (w[0] == "toggle")
        {
            list[i].disabled = !list[i].disabled;
            list[i].name = list[i].disabled ? name + ".disabled" : name.substr(0, name.size() - 9);
        }
        return;
    }
}

std::string Store::runtime() const
{
    if (root.empty())
        return "21.0.5";
    std::string text;
    if (!read_file(root + "/.kit/runtime.txt", text))
        return "";
    const auto p = parse_properties(text);
    const auto it = p.find("java");
    return it == p.end() ? "" : it->second;
}

bool Store::save_offline_profiles() const
{
    if (root.empty())
        return true;
    std::string text;
    std::map<std::string, std::string> cfg;
    if (read_file(root + "/launcher.cfg", text))
        cfg = parse_properties(text);
    std::string profiles;
    for (size_t i = 0; i < offlineProfiles.size(); ++i)
    {
        if (i > 0) profiles += ",";
        profiles += offlineProfiles[i];
    }
    if (profiles.empty())
        cfg.erase("offlineProfiles");
    else
        cfg["offlineProfiles"] = profiles;
    make_dirs(root);
    return write_file_atomic(root + "/launcher.cfg", write_properties(cfg));
}

} // namespace hui::launcher
