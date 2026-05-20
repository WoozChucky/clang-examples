#pragma once

// Cross-DLL export annotation for Engine.dll. dllexport inside Engine's own
// TUs (ENGINE_EXPORTS defined), dllimport everywhere else. Mirrors ECS_API.
#ifndef ENGINE_API
  #ifdef _WIN32
    #ifdef ENGINE_EXPORTS
      #define ENGINE_API __declspec(dllexport)
    #else
      #define ENGINE_API __declspec(dllimport)
    #endif
  #else
    #define ENGINE_API
  #endif
#endif
