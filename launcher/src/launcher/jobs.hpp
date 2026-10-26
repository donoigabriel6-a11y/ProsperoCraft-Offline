// Job client: talks to Kit.java (the job thread inside the JVM) through files in <root>/.kit/.
// SPDX-License-Identifier: GPL-3.0-or-later
//
//   job.cmd       written here:  "<id>\t<command>"            (Kit.java reads and deletes it)
//   job.status    written there: id= state= title= detail= progress=
//   versions.tsv  id, type, releaseTime                       after "list-versions"
//   search.tsv    slug, title, author, downloads, description after "search <type> <query>"
//
// With an empty root (the PC preview) a small simulation answers instead, so every page can be exercised.

#pragma once

#include <string>
#include <vector>

namespace hui::launcher
{

struct VersionRow
{
    std::string id, type, date; // type: release | snapshot | old_beta | old_alpha
};

struct SearchRow
{
    std::string slug, title, author, downloads, description;
    std::string loaders; // mods: "fabric,quilt" (Modrinth categories), "" for other kinds
    std::string span;    // supported Minecraft versions, "1.20 - 1.21.4"
};

// What Launcher.info reads from a mod jar or resource pack ("info mods" / "info resourcepacks").
struct InfoRow
{
    std::string file, loader, mc, version, name, why; // why: non-empty = this mod cannot run on the selected version, and why
};

// One world of the selected instance ("info saves").
struct WorldRow
{
    std::string folder, name, version, mode, icon; // icon: path of a raw RGBA file (width, height as little-endian ints, then pixels), or ""
    long long played = 0;                           // milliseconds since 1970
    long long size = 0;
};

struct JobStatus
{
    std::string id, state = "idle", title, detail; // state: idle | running | done | error
    float progress = -1.0f;                         // 0..1, or -1 when unknown
};

class Jobs
{
  public:
    explicit Jobs(std::string root = "");

    // False while another job runs or the previous command was not picked up yet.
    bool submit(const std::string &command);
    // Call every frame: reads job.status a few times per second.
    void update(float dt);

    bool busy() const
    {
        return !pending_.empty();
    }
    const JobStatus &status() const
    {
        return status_;
    }
    // True exactly once after the submitted job ended. command() / ok() / message() describe it.
    bool take_finished();
    const std::string &command() const
    {
        return last_command_;
    }
    bool ok() const
    {
        return last_ok_;
    }
    const std::string &message() const
    {
        return last_message_;
    }

    // The command that is running (or was submitted last), "" when idle.
    const std::string &active() const
    {
        return pending_command_;
    }

    std::vector<VersionRow> versions() const;
    std::vector<InfoRow> info_rows(const std::string &kind) const;
    std::vector<WorldRow> world_rows() const;
    std::vector<SearchRow> search_results() const;

    bool simulated() const
    {
        return root_.empty();
    }

  private:
    void read_status();
    std::string root_, dir_;
    unsigned counter_ = 0;
    std::string pending_;  // id of the running job, "" when idle
    std::string pending_command_;
    std::string last_command_, last_message_;
    bool last_ok_ = false, finished_ = false;
    JobStatus status_;
    float poll_ = 0.0f, sim_ = 0.0f;
    std::string sim_state_;
};

// Tab-separated lines -> fields, for tests.
std::vector<std::vector<std::string>> parse_tsv(const std::string &text);

} // namespace hui::launcher
