// client.cpp - Ice time client (build on Windows).
//
// Uses the same Time.ice. The proxy string embeds the server's endpoint --
// no naming service, no IOR files. Replace UBUNTU_HOST with the server's
// hostname or IP, and make sure TCP 10000 is open on the Ubuntu firewall.
//
// Build: see ice/README.md (VS project with the ZeroC Ice NuGet package, or
// slice2cpp + cl against the Ice SDK).

#include <Ice/Ice.h>
#include "Time.h"

#include <iostream>

using namespace std;

int main(int argc, char* argv[])
{
    try
    {
        Ice::CommunicatorHolder ich(argc, argv);

        // "<identity>:<transport> -h <host> -p <port>"
        auto base = ich->stringToProxy("TimeSvc:tcp -h UBUNTU_HOST -p 10000");

        // Downcast to the typed proxy; this round-trips to verify the type.
        auto svc = Ice::checkedCast<Demo::TimeSvcPrx>(base);
        if (!svc)
        {
            cerr << "invalid proxy (server not reachable or wrong type)" << endl;
            return 1;
        }

        cout << "Remote local time: " << svc->getLocalTime() << endl;
    }
    catch (const std::exception& ex)
    {
        cerr << "client error: " << ex.what() << endl;
        return 1;
    }
    return 0;
}
