#pragma once

// dllexport when building netlib.dll, dllimport when consuming it. Mirrors ECS_API/ENGINE_API.
#ifndef NETLIB_API
  #ifdef _WIN32
    #ifdef NETLIB_EXPORTS
      #define NETLIB_API __declspec(dllexport)
    #else
      #define NETLIB_API __declspec(dllimport)
    #endif
  #else
    #define NETLIB_API
  #endif
#endif
