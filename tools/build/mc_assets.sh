#!/bin/bash
# mc_assets.sh <PS5 ip> [sounds|all]: download MC 1.20.1's asset index + objects into ~/mc_assets and upload them once over FTP (port 2121)
# to the title folder's mc/assets: FTP shows the title as /data/homebrew/PPSA27272 (the deploy target), which the title sees as /app0, so
# /data/homebrew/PPSA27272/mc/assets == /app0/mc/assets (McBoot starts MC with --assetsDir /app0/mc/assets --assetIndex 5). ftpsrv cannot see /app0 itself.
# Override with env MC_REMOTE=<ftp path> if the title id differs. Re-runs skip files already downloaded/uploaded.
# Default ("sounds"/"game") = every asset object except the other languages: sounds.json + the .ogg files + the real title panorama (client.jar only has 1x1 placeholders). "all" = literally every object.
# Needs: python3 only. NOT run by the assistant (Mojang hosts are unreachable from its sandbox): first run is the test.
set -e
PS5=${1:?usage: bash mc_assets.sh <ps5 ip> [sounds|all]}; SET=${2:-sounds}; V=1.20.1; D=$HOME/mc_assets; mkdir -p $D
python3 - "$V" "$D" "$PS5" "$SET" <<'PY'
import ftplib, hashlib, json, os, sys, urllib.request
v, D, ps5, sel = sys.argv[1:5]
get = lambda u: urllib.request.urlopen(u, timeout=60).read()
m = json.loads(get("https://piston-meta.mojang.com/mc/game/version_manifest_v2.json"))
j = json.loads(get(next(x for x in m["versions"] if x["id"] == v)["url"]))
ai = j["assetIndex"]; print("assetIndex id:", ai["id"], "objects url:", ai["url"])
os.makedirs(D + "/indexes", exist_ok=True)
raw = get(ai["url"]); open("%s/indexes/%s.json" % (D, ai["id"]), "wb").write(raw)
objs = json.loads(raw)["objects"]
pano = sorted(k for k in objs if "panorama" in k or "title/background" in k)
print("panorama/background objects in the index:", pano if pano else "NONE")
# default ("sounds" / "game"): everything except the other languages (minecraft/lang is replaced by the en_us inside client.jar); "all" = literally everything.
# The index lists sounds.json (the event table) and, as pngprobe showed, apparently the real title panorama: client.jar only ships 1x1 placeholders for it.
want = {k: o for k, o in objs.items() if sel == "all" or not (k.startswith("minecraft/lang/") or k.startswith("realms/lang/"))}
print("objects selected:", len(want), "of", len(objs), "(%.1f MB)" % (sum(o["size"] for o in want.values()) / 1e6))
# the index must list only what we ship, otherwise MC logs a missing object per entry; write a trimmed index when a subset is selected
if sel != "all":
    open("%s/indexes/%s.json" % (D, ai["id"]), "w").write(json.dumps({"objects": want}))
need = {}
for k, o in want.items():
    h = o["hash"]; p = "%s/objects/%s/%s" % (D, h[:2], h)
    if not (os.path.exists(p) and os.path.getsize(p) == o["size"]):
        os.makedirs(os.path.dirname(p), exist_ok=True)
        need[h] = p
for n, (h, p) in enumerate(need.items(), 1):
    data = get("https://resources.download.minecraft.net/%s/%s" % (h[:2], h))
    if hashlib.sha1(data).hexdigest() != h: sys.exit("sha1 mismatch for " + h)
    open(p, "wb").write(data)
    if n % 200 == 0 or n == len(need): print("downloaded %d/%d" % (n, len(need)), flush=True)
BASE = os.environ.get("MC_REMOTE", "/data/homebrew/PPSA27272/mc/assets")
print("download done; uploading to ftp://%s:2121%s" % (ps5, BASE))
ftp = ftplib.FTP(); ftp.connect(ps5, 2121, timeout=30); ftp.login(); ftp.set_pasv(True)
parent = os.path.dirname(BASE)
try: print("existing entries in %s: %s" % (parent, ", ".join(sorted(os.path.basename(x) for x in ftp.nlst(parent))) or "(empty)"))
except ftplib.all_errors as e: print("note: cannot list %s (%s); it will be created" % (parent, e))
print("(expect 'saves' / 'crash-reports' there if this folder is the title's /app0/mc)")
made = set()
def mk(path):
    cur = ""
    for part in [x for x in path.split("/") if x]:
        cur += "/" + part
        if cur in made: continue
        try: ftp.mkd(cur)
        except ftplib.error_perm: pass
        made.add(cur)
checked = set()
def up(local, remote):
    d = os.path.dirname(remote); mk(d)
    if d not in checked:   # fail loudly instead of a bare 550 on STOR
        try: ftp.cwd(d); ftp.cwd("/")
        except ftplib.all_errors as e: sys.exit("cannot create or enter %s over FTP: %s" % (d, e))
        checked.add(d)
    try:
        ftp.voidcmd("TYPE I")
        if ftp.size(remote) == os.path.getsize(local): return 0
    except Exception: pass
    with open(local, "rb") as f: ftp.storbinary("STOR " + remote, f)
    return 1
sent = 0; allf = [("%s/indexes/%s.json" % (D, ai["id"]), BASE + "/indexes/%s.json" % ai["id"])]
for o in want.values(): h = o["hash"]; allf.append(("%s/objects/%s/%s" % (D, h[:2], h), BASE + "/objects/%s/%s" % (h[:2], h)))
for n, (l, r) in enumerate(allf, 1):
    sent += up(l, r)
    if n % 200 == 0 or n == len(allf): print("uploaded %d/%d (sent %d)" % (n, len(allf), sent), flush=True)
ftp.quit(); print("done")
PY
