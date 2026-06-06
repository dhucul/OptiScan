// ============================================================================
// DiscTypes.h - Compatibility header (includes all split type headers)
//
// This file now serves as a compatibility layer that includes all the split
// headers. Existing code that includes DiscTypes.h will continue to work.
// New code should include only the specific headers needed.
// ============================================================================
#pragma once

// Include all split headers for backward compatibility
#include "Constants.h"
#include "CDStructures.h"
#include "SecureRipTypes.h"
#include "RecoveryTypes.h"
#include "ErrorTypes.h"
#include "AnalysisTypes.h"
#include "ScanResults.h"
#include "DriveTypes.h"
#include "FingerprintTypes.h"

// All types from the original DiscTypes.h are now available through the includes above.