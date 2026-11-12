#pragma once
// Stand-in for the module builder's generated logos_sdk.h: the same
// `modules().delivery_module` surface the impl uses, backed by an in-process
// bus instead of the Logos delivery network. Every FakeDelivery that
// subscribed to a topic gets each message sent on it (including the sender —
// gossipsub echoes too), on a separate thread like the real event path.

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "logos_result.h"

struct FakeDelivery {
    using MessageCb = std::function<void(const std::string&, const std::string&, const std::vector<uint8_t>&,
                                         const std::string&, int64_t)>;
    using ConnectionCb = std::function<void(const std::string&, int64_t)>;

    static std::mutex& busMu() { static std::mutex m; return m; }
    static std::set<FakeDelivery*>& bus() { static std::set<FakeDelivery*> s; return s; }

    FakeDelivery() { std::lock_guard<std::mutex> lk(busMu()); bus().insert(this); }
    ~FakeDelivery() { std::lock_guard<std::mutex> lk(busMu()); bus().erase(this); }

    StdLogosResult createNode(const std::string&) { created = true; return {true, "ok", ""}; }
    StdLogosResult start() { started = true; return {true, "started", ""}; }
    StdLogosResult stop() { started = false; return {true, "stopped", ""}; }
    StdLogosResult subscribe(const std::string& topic)
    {
        std::lock_guard<std::mutex> lk(mu);
        topics.insert(topic);
        return {true, topic, ""};
    }
    StdLogosResult unsubscribe(const std::string& topic)
    {
        std::lock_guard<std::mutex> lk(mu);
        topics.erase(topic);
        return {true, topic, ""};
    }
    StdLogosResult send(const std::string& topic, const std::vector<uint8_t>& payload)
    {
        std::vector<FakeDelivery*> targets;
        {
            std::lock_guard<std::mutex> lk(busMu());
            for (FakeDelivery* d : bus()) {
                std::lock_guard<std::mutex> lk2(d->mu);
                if (d->started && d->topics.count(topic)) targets.push_back(d);
            }
        }
        ++sent;
        for (FakeDelivery* d : targets) {
            MessageCb cb;
            {
                std::lock_guard<std::mutex> lk(d->mu);
                cb = d->onMessage;
            }
            if (!cb) continue;
            std::thread([cb, topic, payload] {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                using namespace std::chrono;
                cb("hash", topic, payload, "live", duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
            }).detach();
        }
        return {true, "sent", ""};
    }
    void onMessageReceived(MessageCb cb) { std::lock_guard<std::mutex> lk(mu); onMessage = std::move(cb); }
    void onConnectionStateChanged(ConnectionCb cb) { std::lock_guard<std::mutex> lk(mu); onConnection = std::move(cb); }

    std::mutex mu;
    std::set<std::string> topics;
    MessageCb onMessage;
    ConnectionCb onConnection;
    bool created = false, started = false;
    int sent = 0;
};

struct LogosModules {
    FakeDelivery delivery_module;
};
