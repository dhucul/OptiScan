#pragma once
#include "ScsiDrive.h"

// Feature lifetime covers normal exits and exceptions from the writer.
struct WriteFeatureGuard {
    ScsiDrive& drive;
    bool& test;
    bool& variRec;
    ~WriteFeatureGuard() {
        if (test) drive.SetPlextorTestWrite(false);
        if (variRec) drive.SetVariRecCD(false, 0);
    }
};
