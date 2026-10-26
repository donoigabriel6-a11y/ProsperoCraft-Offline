// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio/ps5_out.hpp"

#include <atomic>
#include <cstdint>
#include <pthread.h>
#include <unistd.h>

extern "C"
{
int sceAudioOutInit(void);
int sceAudioOutOpen(int user_id, int type, int index, unsigned len, unsigned freq, unsigned param);
int sceAudioOutOutput(int handle, const void *ptr);
int sceAudioOutClose(int handle) __attribute__((weak));
}

namespace hui::audio
{

namespace
{

constexpr int kGrain = 256; // frames per sceAudioOut call (valid: 256, 512, 768 ...): about 5 ms, the console queues ~3 of them

struct State
{
    Mixer *mixer = nullptr;
    int handle = -1;
    std::atomic<bool> run{false};
    pthread_t thread{};
    bool started = false;
};

void *render_thread(void *arg)
{
    State *s = static_cast<State *>(arg);
    alignas(256) static std::int16_t buffer[kGrain * 2];
    while (s->run.load(std::memory_order_acquire))
    {
        s->mixer->render(buffer, kGrain);
        sceAudioOutOutput(s->handle, buffer); // blocks until the console wants the next grain
    }
    return nullptr;
}

} // namespace

bool ConsoleOut::start(Mixer &mixer)
{
    if (state_)
        return true;
    sceAudioOutInit();
    const int handle = sceAudioOutOpen(0xFF, 0, 0, kGrain, kSampleRate, 1); // system user, MAIN port, S16 stereo (what ps5al.inc uses)
    if (handle <= 0)
        return false;
    State *s = new State;
    s->mixer = &mixer;
    s->handle = handle;
    s->run.store(true, std::memory_order_release);
    if (pthread_create(&s->thread, nullptr, render_thread, s) != 0)
    {
        if (sceAudioOutClose)
            sceAudioOutClose(handle);
        delete s;
        return false;
    }
    s->started = true;
    state_ = s;
    return true;
}

void ConsoleOut::stop()
{
    State *s = static_cast<State *>(state_);
    if (!s)
        return;
    s->run.store(false, std::memory_order_release);
    if (s->started)
        pthread_join(s->thread, nullptr);
    if (sceAudioOutClose)
        sceAudioOutClose(s->handle);
    delete s;
    state_ = nullptr;
}

} // namespace hui::audio
