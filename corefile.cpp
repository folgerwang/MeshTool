#include "corefile.h"

#include <fstream>
#include <sstream>
#include <filesystem>


namespace core
{
char* LoadFileToMemory(const string& file_name, uint32_t& length)
{
    char* buffer = nullptr;
    ifstream inFile;
    inFile.open(file_name, ifstream::binary);

    if (inFile)
    {
        inFile.seekg(0, inFile.end);
        length = static_cast<uint32_t>(inFile.tellg());
        inFile.seekg(0, inFile.beg);

        // allocate memory:
        buffer = new char[length + 1];

        // read data as a block:
        inFile.read(buffer, length);
        buffer[length] = '\0';
        inFile.close();
    }

    return buffer;
}
}

