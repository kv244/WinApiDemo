// server.cpp - Ice time server (build on Ubuntu).
//
//   slice2cpp Time.ice
//   g++ -std=c++17 server.cpp Time.cpp -lIce++11 -lpthread -o timeserver
//   ./timeserver          # listens on TCP 10000, all interfaces
//
// Ice's binary protocol runs directly over TCP -- no HTTP. The object adapter
// owns the endpoint; the identity "TimeSvc" is what the client's proxy names.

#include <Ice/Ice.h>
#include "Time.h"

#include <ctime>
#include <cstdio>
#include <memory>
#include <string>
#include <iostream>

using namespace std;

// The servant: implements the Slice-generated Demo::TimeSvc skeleton.
class TimeSvcI final : public Demo::TimeSvc
{
public:
    string getLocalTime(const Ice::Current&) override
    {
        time_t t = time(nullptr);
        tm lt{};
        localtime_r(&t, &lt);                 // POSIX; thread-safe localtime
        char buf[16];
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d",
                 lt.tm_hour, lt.tm_min, lt.tm_sec);
        return string(buf);
    }
};

int main(int argc, char* argv[])
{
    try
    {
        // RAII wrapper: initializes and destroys the communicator (the ORB).
        Ice::CommunicatorHolder ich(argc, argv);

        // Create an adapter bound to TCP port 10000 on every interface.
        auto adapter = ich->createObjectAdapterWithEndpoints(
            "TimeAdapter", "tcp -p 10000");

        // Register the servant under the identity the client will ask for.
        adapter->add(make_shared<TimeSvcI>(), Ice::stringToIdentity("TimeSvc"));
        adapter->activate();

        cout << "TimeSvc listening on tcp -p 10000. Ctrl+C to stop." << endl;
        ich->waitForShutdown();
    }
    catch (const std::exception& ex)
    {
        cerr << "server error: " << ex.what() << endl;
        return 1;
    }
    return 0;
}
