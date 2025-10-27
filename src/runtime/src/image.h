#pragma once

void* image_load(const char* path, int* width, int* height, int* channels);
void image_free(void* imageData);