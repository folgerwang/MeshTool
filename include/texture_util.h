#pragma once

#include <string>
#include "coremath.h"
#include "coretexture.h"

using namespace std;

void LoadTextureFromFile(const string& file_name, core::Texture2DInfo* tex_info);
