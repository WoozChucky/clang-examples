#include "image.h"

#include <lib.h>
#define STB_IMAGE_IMPLEMENTATION
#include "../../editor/src/stb_image.h"

void* image_load(const char* path, int* width, int* height, int* channels)
{
  stbi_uc* data = stbi_load(path, width, height, channels, 4);
  if(!data)
  {
    SM_ERROR("Failed to load image: %s", path);
    return nullptr;
  }
  return data;
}

void image_free(void* imageData)
{
  if(imageData)
  {
    stbi_image_free(imageData);
  }
}