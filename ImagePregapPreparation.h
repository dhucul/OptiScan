#pragma once
#include "ArtifactTransaction.h"
#include <memory>

struct PreparedImageSource {
    std::wstring sourceBin, binFile, cueFile, subFile;
    bool normalized = false;
    std::unique_ptr<ArtifactTransaction> temporary;
};

// Materialize generated gaps and explicitly referenced separate gap audio
// before any media can be erased. Plain INDEX-based images pass through.
bool PrepareImagePregaps(const std::wstring& cueFile, PreparedImageSource& output, std::string& error);
