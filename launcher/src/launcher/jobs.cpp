// SPDX-License-Identifier: GPL-3.0-or-later
#include "launcher/jobs.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>

namespace hui::launcher
{

namespace
{

bool read_text(const std::string &path, std::string *out)
{
    std::FILE *f = std::fopen(path.c_str(), "rb");
    if (!f)
        return false;
    out->clear();
    char buf[4096];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
        out->append(buf, n);
    std::fclose(f);
    return true;
}

bool write_atomic(const std::string &path, const std::string &text)
{
    const std::string tmp = path + ".tmp";
    std::FILE *f = std::fopen(tmp.c_str(), "wb");
    if (!f)
        return false;
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    std::fclose(f);
    if (!ok)
        return false;
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

bool exists(const std::string &path)
{
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
}

} // namespace

std::vector<std::vector<std::string>> parse_tsv(const std::string &text)
{
    std::vector<std::vector<std::string>> rows;
    std::size_t pos = 0;
    while (pos < text.size())
    {
        std::size_t end = text.find('\n', pos);
        if (end == std::string::npos)
            end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;
        std::vector<std::string> fields;
        std::size_t s = 0;
        for (;;)
        {
            const std::size_t t = line.find('\t', s);
            if (t == std::string::npos)
            {
                fields.push_back(line.substr(s));
                break;
            }
            fields.push_back(line.substr(s, t - s));
            s = t + 1;
        }
        rows.push_back(std::move(fields));
    }
    return rows;
}

Jobs::Jobs(std::string root) : root_(std::move(root))
{
    if (!root_.empty())
        dir_ = root_ + "/.kit";
}

bool Jobs::submit(const std::string &command)
{
    if (!pending_.empty())
        return false;
    ++counter_;
    const std::string id = std::to_string(counter_);
    if (!root_.empty())
    {
        if (exists(dir_ + "/job.cmd"))
            return false;
        if (!write_atomic(dir_ + "/job.cmd", id + "\t" + command + "\n"))
        {
            --counter_;
            return false;
        }
    }
    pending_ = id;
    pending_command_ = command;
    status_ = JobStatus{};
    status_.id = id;
    status_.state = "running";
    status_.title = "Waiting for the downloader";
    poll_ = 0.0f;
    sim_ = 0.0f;
    return true;
}

void Jobs::read_status()
{
    std::string text;
    if (!read_text(dir_ + "/job.status", &text))
        return;
    JobStatus s;
    std::size_t pos = 0;
    while (pos < text.size())
    {
        std::size_t end = text.find('\n', pos);
        if (end == std::string::npos)
            end = text.size();
        const std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "id")
            s.id = v;
        else if (k == "state")
            s.state = v;
        else if (k == "title")
            s.title = v;
        else if (k == "detail")
            s.detail = v;
        else if (k == "progress")
            s.progress = static_cast<float>(std::atof(v.c_str()));
    }
    if (s.id != pending_)
        return; // a stale status of an earlier job
    status_ = s;
}

void Jobs::update(float dt)
{
    if (pending_.empty())
        return;
    if (root_.empty())
    {
        // Simulation: three seconds of progress, then success ("fail" in the command makes it an error).
        sim_ += dt;
        const auto starts = [this](const char *prefix) { return pending_command_.rfind(prefix, 0) == 0; };
        const bool removal = starts("remove") || starts("toggle") || starts("instance delete") || starts("info") || starts("world");
        const bool quick = starts("list-versions") || starts("search") || removal;
        const float dur = quick ? 0.8f : 3.0f;
        status_.title = removal ? "Working (preview)" : "Installing (preview)";
        status_.state = "running";
        status_.progress = removal ? -1.0f : std::min(1.0f, sim_ / dur);
        status_.detail = removal ? "Simulated" : "Simulated download  " + std::to_string(static_cast<int>(sim_ / dur * 120)) + " / 120 files";
        if (sim_ >= dur)
        {
            status_.state = (pending_command_.find("fail") != std::string::npos || (std::getenv("HUI_SIM_FAIL") && !quick)) ? "error" : "done";
            status_.detail = status_.state == "error" ? "Simulated failure: could not reach the download server (preview)" : "Done: " + pending_command_;
            status_.progress = 1.0f;
            if (removal && status_.state == "done")
                status_.detail = "Done";
        }
    }
    else
    {
        poll_ -= dt;
        if (poll_ > 0.0f)
            return;
        poll_ = 0.2f;
        read_status();
    }
    if (status_.state == "done" || status_.state == "error")
    {
        last_command_ = pending_command_;
        last_ok_ = status_.state == "done";
        last_message_ = status_.detail;
        finished_ = true;
        pending_.clear();
    }
}

bool Jobs::take_finished()
{
    const bool f = finished_;
    finished_ = false;
    return f;
}

std::vector<VersionRow> Jobs::versions() const
{
    std::vector<VersionRow> out;
    if (root_.empty())
    {
        static const char *demo[][3] = {{"1.21.4", "release", "2024-12-03"}, {"1.21.1", "release", "2024-08-08"},
                                        {"1.20.4", "release", "2023-12-07"}, {"1.20.1", "release", "2023-06-12"},
                                        {"1.19.4", "release", "2023-03-14"}, {"25w05a", "snapshot", "2025-01-29"},
                                        {"24w46a", "snapshot", "2024-11-13"}, {"1.8.9", "release", "2015-12-09"}};
        for (const auto &d : demo)
            out.push_back({d[0], d[1], d[2]});
        return out;
    }
    std::string text;
    if (!read_text(dir_ + "/versions.tsv", &text))
        return out;
    for (const auto &r : parse_tsv(text))
        if (r.size() >= 2)
            out.push_back({r[0], r[1], r.size() > 2 ? r[2].substr(0, 10) : ""});
    return out;
}

std::vector<SearchRow> Jobs::search_results() const
{
    std::vector<SearchRow> out;
    if (root_.empty())
    {
        static const char *demo[][5] = {
            {"sodium", "Sodium", "jellysquid3", "58.2M", "A modern rendering engine for Minecraft which greatly improves performance"},
            {"lithium", "Lithium", "jellysquid3", "41.0M", "No-compromises game optimization mod"},
            {"iris", "Iris Shaders", "coderbot", "33.5M", "A modern shaders mod compatible with existing OptiFine shader packs"},
            {"fabric-api", "Fabric API", "modmuss50", "99.9M", "Core API module providing common hooks and intercompatibility measures"},
            {"ferritecore", "FerriteCore", "malte0811", "30.1M", "Memory usage optimizations"}};
        for (const auto &d : demo)
            out.push_back({d[0], d[1], d[2], d[3], d[4], "fabric,quilt", "1.20 - 1.21.4"});
        return out;
    }
    std::string text;
    if (!read_text(dir_ + "/search.tsv", &text))
        return out;
    for (const auto &r : parse_tsv(text))
        if (r.size() >= 2)
            out.push_back({r[0], r[1], r.size() > 2 ? r[2] : "", r.size() > 3 ? r[3] : "", r.size() > 4 ? r[4] : "", r.size() > 5 ? r[5] : "",
                           r.size() > 6 ? r[6] : ""});
    return out;
}

std::vector<InfoRow> Jobs::info_rows(const std::string &kind) const
{
    std::vector<InfoRow> out;
    if (root_.empty())
    {
        if (kind == "mods")
        {
            out.push_back({"sodium-fabric-0.5.8+mc1.20.1.jar", "Fabric", "1.20.x", "0.5.8", "Sodium", ""});
            out.push_back({"iris-fabric-1.6.17+mc1.20.1.jar", "Fabric", "1.20 - <1.20.5", "1.6.17", "Iris Shaders", ""});
            out.push_back({"fabric-api-0.92.2+1.20.1.jar", "Fabric", "1.20.1", "0.92.2", "Fabric API", ""});
            out.push_back({"modmenu-7.2.2.jar.disabled", "Fabric", "1.20+", "7.2.2", "Mod Menu", ""});
            out.push_back({"sodium-fabric-0.8.13+mc1.21.1.jar", "Fabric", "26.4-snapshot-4+", "0.8.13", "Sodium", "needs Minecraft 26.4-snapshot-4+"});
            out.push_back({"lithium-fabric-mc1.20.1-0.11.2.jar", "Quilt", "1.20.x", "0.11.2", "Lithium", "built for Quilt"});
        }
        else if (kind == "resourcepacks")
        {
            out.push_back({"Faithful 32x - 1.20.1.zip", "", "1.20 - 1.20.1", "format 15", "", ""});
            out.push_back({"Fresh Animations v1.9.zip", "", "1.21 - 1.21.1", "format 34", "", ""});
        }
        else if (kind == "datapacks")   // preview: one row per pack and world (pack, world folder, world name, bytes, "1" = on, world version)
        {
            const char *packs[] = {"Veinminer.zip", "Terralith_1.4.zip", "Incendium.zip", "Better Mineshafts"};
            const char *sizes[] = {"184000", "9400000", "5100000", "2300000"};
            const char *folders[] = {"New World", "New World (1)", "Hardcore", "Skyblock Run"};
            const char *names[] = {"Survival island", "Creative test", "Hardcore run", "Skyblock with a very long world name"};
            const char *vers[] = {"1.21.1", "1.20.1", "1.8.9", "1.21.1"};
            for (int p = 0; p < 4; ++p)
                for (int w = 0; w < 4; ++w)
                    out.push_back({packs[p], folders[w], names[w], sizes[p], (p + w) % 3 == 0 ? "1" : "0", vers[w]});
        }
        return out;
    }
    std::string text;
    if (!read_text(dir_ + "/info-" + kind + ".tsv", &text))
        return out;
    for (const auto &r : parse_tsv(text))
    {
        if (r.empty())
            continue;
        const auto at = [&r](std::size_t i) { return r.size() > i ? r[i] : std::string(); };
        out.push_back({r[0], at(1), at(2), at(3), at(4), at(5)});
    }
    return out;
}

std::vector<WorldRow> Jobs::world_rows() const
{
    std::vector<WorldRow> out;
    if (root_.empty())
    {
        out.push_back({"New World", "Survival island", "1.21.1", "Survival", "", 1767000000000LL, 48200000});
        out.push_back({"New World (1)", "Creative test", "1.20.1", "Creative", "", 1760000000000LL, 6100000});
        out.push_back({"Hardcore", "Hardcore run", "1.8.9", "Hardcore", "", 1750000000000LL, 21000000});
        out.push_back({"Skyblock Run", "Skyblock with a very long world name", "1.21.1", "Survival", "", 1740000000000LL, 3100000});
        return out;
    }
    std::string text;
    if (!read_text(dir_ + "/info-saves.tsv", &text))
        return out;
    for (const auto &r : parse_tsv(text))
    {
        if (r.size() < 2)
            continue;
        const auto at = [&r](std::size_t i) { return r.size() > i ? r[i] : std::string(); };
        WorldRow w;
        w.folder = r[0];
        w.name = at(1);
        w.version = at(2);
        w.mode = at(3);
        w.played = std::atoll(at(4).c_str());
        w.size = std::atoll(at(5).c_str());
        w.icon = at(6);
        out.push_back(std::move(w));
    }
    return out;
}

} // namespace hui::launcher
