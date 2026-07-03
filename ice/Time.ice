// Time.ice - Slice interface for the remote time service.
// Compile:  slice2cpp Time.ice   ->  Time.h, Time.cpp
//
// Slice is Ice's IDL. slice2cpp generates a proxy (client side) and a servant
// base class (server side) from this one file; both ends share it.

#pragma once

module Demo
{
    interface TimeSvc
    {
        // Returns the server's local time as "HH:MM:SS".
        string getLocalTime();
    }
}
