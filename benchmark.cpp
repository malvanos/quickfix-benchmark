#include "quickfix/Application.h"
#include "quickfix/MessageCracker.h"
#include "quickfix/Session.h"
#include "quickfix/SessionSettings.h"
#include "quickfix/FileStore.h"
#include "quickfix/SocketInitiator.h"
#include "quickfix/SocketAcceptor.h"
#include "quickfix/fix44/NewOrderSingle.h"
#include "quickfix/fix44/ExecutionReport.h"

#include <chrono>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>
#include <condition_variable>
#include <algorithm>
#include <atomic>
#include <pthread.h>
#include <sched.h>

using namespace std::chrono;
using Clock = high_resolution_clock;

const int TOTAL_MESSAGES = 10000;
std::vector<long> latencies;
std::vector<Clock::time_point> send_timestamps(TOTAL_MESSAGES);
std::mutex mtx;
std::condition_variable cv;
std::atomic<int> received = 0;

// Function to set thread affinity
void set_thread_affinity(std::thread& thread, int core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);

    int rc = pthread_setaffinity_np(thread.native_handle(), sizeof(cpu_set_t), &cpuset);
    if (rc != 0) {
        std::cerr << "Error setting thread affinity: " << strerror(rc) << "\n";
    }
}

class AcceptorApp : public FIX::Application, public FIX::MessageCracker {
public:
    void onCreate(const FIX::SessionID&) override {}
    void onLogon(const FIX::SessionID&) override {}
    void onLogout(const FIX::SessionID&) override {}
    void toAdmin(FIX::Message&, const FIX::SessionID&) override {}
    void fromAdmin(const FIX::Message&, const FIX::SessionID&) override {}
    void toApp(FIX::Message&, const FIX::SessionID&) override {}

    void fromApp(const FIX::Message& message, const FIX::SessionID& sessionID) override {
        crack(message, sessionID);
    }

    void onMessage(const FIX44::NewOrderSingle& order, const FIX::SessionID& sessionID) override {
        FIX::ClOrdID clordid;
        order.get(clordid);
        std::string id = clordid.getValue();
        std::string id2 = id;

        FIX44::ExecutionReport exec;
        exec.set(FIX::OrderID(id));
        exec.set(FIX::ExecID(id2));
        exec.set(FIX::ExecType(FIX::ExecType_TRADE));
        exec.set(FIX::OrdStatus(FIX::OrdStatus_FILLED));
        exec.set(clordid);
        exec.set(FIX::Side(order.getField(54)[0]));  // or extract with `order.get()`
        exec.set(FIX::LeavesQty(0));
        exec.set(FIX::CumQty(100));
        exec.set(FIX::AvgPx(1.0));

        FIX::Session::sendToTarget(exec, sessionID);
    }
};

class InitiatorApp : public FIX::Application, public FIX::MessageCracker {
public:
    void onCreate(const FIX::SessionID& sid) override {
        sessionID = sid;
    }

    void onLogon(const FIX::SessionID&) override {
        std::thread sender_thread([this]() {
            for (int i = 0; i < TOTAL_MESSAGES; ++i) {
                FIX44::NewOrderSingle order(
                    FIX::ClOrdID(std::to_string(i)),
                    FIX::Side(FIX::Side_BUY),
                    FIX::TransactTime(),
                    FIX::OrdType(FIX::OrdType_MARKET)
                );
                order.set(FIX::OrderQty(100));
                order.set(FIX::Symbol("AAPL"));

                send_timestamps[i] = Clock::now();

                FIX::Session::sendToTarget(order, sessionID);
                std::this_thread::sleep_for(std::chrono::microseconds(10));
            }
        });

        // Pin the sender thread to core 4
        set_thread_affinity(sender_thread, 4);
        sender_thread.detach();
    }

    void onLogout(const FIX::SessionID&) override {}
    void toAdmin(FIX::Message&, const FIX::SessionID&) override {}
    void fromAdmin(const FIX::Message&, const FIX::SessionID&) override {}
    void toApp(FIX::Message&, const FIX::SessionID&) override {}

    void fromApp(const FIX::Message& message, const FIX::SessionID& sessionID) override {
        crack(message, sessionID);
    }

    void onMessage(const FIX44::ExecutionReport& report, const FIX::SessionID&) override {
        FIX::ClOrdID clordid;
        report.get(clordid);
        int id = std::stoi(clordid.getValue());
        auto now = Clock::now();

        long latency = duration_cast<nanoseconds>(now - send_timestamps[id]).count();
        {
            std::lock_guard<std::mutex> lock(mtx);
            latencies.push_back(latency);
            received++;
        }

        if (received >= TOTAL_MESSAGES) {
            cv.notify_one();
        }
    }

private:
    FIX::SessionID sessionID;
};

void print_stats() {
    std::sort(latencies.begin(), latencies.end());

    // Remove 5% of the best and worst latencies
    size_t drop_count = latencies.size() * 5 / 100;
    if (latencies.size() > 2 * drop_count) {
        latencies.erase(latencies.begin(), latencies.begin() + drop_count); // Remove best 5%
        latencies.erase(latencies.end() - drop_count, latencies.end());    // Remove worst 5%
    }

    long min = latencies.front() / 1000; // Convert to microseconds
    long max = latencies.back() / 1000;  // Convert to microseconds
    double avg = std::accumulate(latencies.begin(), latencies.end(), 0.0) / latencies.size() / 1000; // Convert to microseconds
    long p50 = latencies[latencies.size() / 2] / 1000; // Convert to microseconds
    long p99 = latencies[latencies.size() * 99 / 100] / 1000; // Convert to microseconds

    std::cout << "Messages: " << latencies.size() << "\n";
    std::cout << "Min: " << min << " µs\n";
    std::cout << "Max: " << max << " µs\n";
    std::cout << "Avg: " << avg << " µs\n";
    std::cout << "P50: " << p50 << " µs\n";
    std::cout << "P99: " << p99 << " µs\n";
}

int main() {
    try {
        FIX::SessionSettings settingsAcceptor("acceptor.cfg");
        FIX::FileStoreFactory storeFactoryAcceptor(settingsAcceptor);

        AcceptorApp acceptorApp;
        FIX::SocketAcceptor acceptor(acceptorApp, storeFactoryAcceptor, settingsAcceptor);

        FIX::SessionSettings settingsInitiator("initiator.cfg");
        FIX::FileStoreFactory storeFactoryInitiator(settingsInitiator);
        InitiatorApp initiatorApp;
        FIX::SocketInitiator initiator(initiatorApp, storeFactoryInitiator, settingsInitiator);

        // Start acceptor thread and pin it to core 0
        std::thread acceptor_thread([&]() { acceptor.start(); });
        set_thread_affinity(acceptor_thread, 0);

        // Start initiator thread
        std::thread initiator_thread([&]() { initiator.start(); });
        set_thread_affinity(initiator_thread, 4);

        acceptor_thread.join();
        initiator_thread.join();

        std::unique_lock<std::mutex> lock(mtx);
        cv.wait(lock, [] { return received >= TOTAL_MESSAGES; });


        print_stats();

        initiator.stop();
        acceptor.stop();
    } catch (std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
    }
    return 0;
}

