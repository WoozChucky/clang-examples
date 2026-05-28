#pragma once
#include <cstddef>

// Native Windows "open file" dialog. Returns true if a file was chosen (path written to outPath).
namespace EditorFileDialog {
    bool Open(char* outPath, size_t outPathSize, const char* filter);
}
