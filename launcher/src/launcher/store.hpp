// Launcher data: instances and settings kept in the files the Java side (Launcher.java) reads and writes.
// SPDX-License-Identifier: GPL-3.0-or-later
//
//   <root>/launcher.cfg                  instance=<id>, account=<id>, ui=menu|auto|off ...
//   <root>/instances/<id>/instance.cfg   name, version, loader, xmx, cpus, jitThreads, gc, account ...
//   <root>/versions/<id>/                installed versions (a jar, or the .installed marker of a Fabric/Quilt profile)
//
// Plain key=value lines, sorted on save, written through a temp file: the same format on both sides.

#pragma once

#include <map>
#include <string>
#include <vector>

namespace hui::launcher
{

struct Instance
{
    std::string id;
    std::map<std::string, std::string> props;

    std::string get(const std::string &key, const std::string &fallback = "") const;
    int geti(const std::string &key, int fallback) const;
    void set(const std::string &key, const std::string &value);
    void seti(const std::string &key, int value);

    std::string name() const
    {
        return get("name", id == "default" ? "Default" : id);
    }
    std::string version() const
    {
        return get("version", "");
    }
    std::string loader() const // "", "fabric" or "quilt"
    {
        return get("loader", "");
    }
};

// Last "x.y.z" of a version id: "fabric-loader-0.19.5-1.21.1" -> "1.21.1" (plain versions are returned as they are).
std::string game_version(const std::string &id);
std::string loader_kind(const std::string &id); // "fabric-loader-0.16.9-1.21.1" -> "fabric"; "1.20.1-forge-47.2.0" -> "forge"; "neoforge-21.1.172" -> "neoforge"; a plain version -> ""
// Percent-encoding for names inside job commands (a job is one whitespace-separated line; Launcher.dec decodes it).
std::string url_encode(const std::string &s);
// "12.4 MB"
std::string size_text(long long bytes);

// One file or folder of an instance's mods / resource packs / shader packs.
struct Item
{
    std::string name;      // the name inside its folder (what a job needs)
    long long size = 0;    // bytes; a folder counts all its files
    bool disabled = false; // mods: X.jar.disabled (switched off by hand)
    bool aside = false;    // mods-disabled/: set aside because it is built for another Minecraft version
};

class Store
{
  public:
    std::string root;
    std::vector<Instance> instances; // "default" first
    std::vector<std::string> versions; // installed, sorted
    std::string current;               // launcher.cfg instance=
    std::string account = "offline";   // launcher.cfg account=
    std::string username = "Player";   // launcher.cfg username=: the offline player name (an instance.cfg username= overrides it)
    std::vector<std::string> offlineProfiles; // launcher.cfg offlineProfiles=: comma-separated offline profile names
    std::string selected;              // versions/selected.txt: what the default instance runs when it names no version

    // Reads <root>; a missing folder yields the default instance only.
    static Store load(const std::string &root);
    // Sample data for the PC preview.
    static Store demo();

    int find(const std::string &id) const; // index in instances, or -1
    // Number of .jar files in the instance's mods folder.
    int mod_count(const Instance &instance) const;
    // Writes instances/<id>/instance.cfg. False when the write failed.
    bool save(const Instance &instance) const;
    bool select(const std::string &id);   // launcher.cfg instance=
    // versions/selected.txt: what the default instance runs when it names no version ("" = none).
    bool set_selected(const std::string &id);
    bool set_config(const std::string &key, const std::string &value); // any launcher.cfg key; empty value removes it
    std::string config(const std::string &key) const;
    // Drops an instance from memory (and selects "default" if it was current). The files are deleted by the
    // `instance delete <id>` job; Store::load no longer lists the instance once its folder is gone.
    void forget(const std::string &id);
    std::string game_dir(const Instance &instance) const;

    // What the instance runs: its own version, else versions/selected.txt for the default instance.
    std::string effective_version(const Instance &instance) const;
    // Names of the instances and loader profiles that still need an installed version (empty: safe to delete).
    std::vector<std::string> version_users(const std::string &id) const;
    long long version_size(const std::string &id) const;
    // folder: "mods", "mods-disabled", "resourcepacks" or "shaderpacks" inside the instance's game folder.
    std::vector<Item> list_items(const Instance &instance, const std::string &folder) const;
    // Preview only (empty root): applies a finished remove/toggle job to the sample data.
    void demo_apply(const std::string &command);
    // The Java this title runs on (from .kit/runtime.txt), "" when unknown.
    std::string runtime() const;
    // Save offline profiles to launcher.cfg
    bool save_offline_profiles() const;

  private:
    std::map<std::string, std::vector<Item>> demo_items_;
};

// The two file helpers, exposed for tests.
std::map<std::string, std::string> parse_properties(const std::string &text);
std::string write_properties(const std::map<std::string, std::string> &props);

} // namespace hui::launcher
