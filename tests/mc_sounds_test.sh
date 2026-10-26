#!/bin/bash
# Launcher UI sounds: builds an asset index + an Ogg object with ffmpeg, then checks that load_minecraft_sounds decodes it (stb_vorbis from third_party, the console build uses LWJGL's copy) and the mixer renders it.
cd "$(dirname "$0")"; T=$PWD; D=$(mktemp -d)
command -v ffmpeg >/dev/null || { echo "skipped (no ffmpeg)"; exit 0; }
mkdir -p $D/root/.kit $D/root/assets/indexes $D/root/assets/objects/ab
ffmpeg -loglevel error -y -f lavfi -i "sine=frequency=880:duration=0.3" -ar 44100 -ac 1 -c:a libvorbis $D/click.ogg || { echo "skipped (no libvorbis)"; exit 0; }
H=abcdef0123456789abcdef0123456789abcdef01; cp $D/click.ogg $D/root/assets/objects/ab/$H
echo "{\"objects\": {\"minecraft/sounds/random/click.ogg\": {\"hash\": \"$H\", \"size\": 1}}}" > $D/root/assets/indexes/12.json
echo "$D/root/assets/indexes/12.json" > $D/root/.kit/sounds.txt
S=$T/../kit_launcher/src
gcc -w -O1 -c third_party/stb_vorbis.c -o $D/sv.o && g++ -std=c++20 -O1 -w -I$S mc_sounds_test.cpp $S/audio/mc_sounds.cpp $S/audio/cues.cpp $S/audio/mixer.cpp $S/audio/wav.cpp $S/core/save_file.cpp $D/sv.o -o $D/t && $D/t $D/root
