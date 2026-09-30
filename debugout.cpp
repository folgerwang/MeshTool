#include "debugout.h"
#include <cstdio>

namespace core
{
void output_debug_info(const string& message_head, const string& message_body)
{
    fprintf(stderr, "[%s] %s\n", message_head.c_str(), message_body.c_str());
}
}
