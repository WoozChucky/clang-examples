#include "lib.h"
#include "platform.h"

#include <Windows.h>
#include <xaudio2.h>
#include <atomic>
#include <string.h>

// #############################################################################
//                           Windows Structs
// #############################################################################
struct xAudioVoice : IXAudio2VoiceCallback
{
  IXAudio2SourceVoice* voice;
  SoundOptions options;
  float fadeTimer;
  char* soundPath;

  int playing;

	void OnStreamEnd() noexcept
	{
		voice->Stop();
    playing = false;
	}

	void OnBufferStart(void * pBufferContext) noexcept
	{
    playing = true;
	}

	void OnVoiceProcessingPassEnd() noexcept {}
	void OnVoiceProcessingPassStart(UINT32 SamplesRequired) noexcept {}
	void OnBufferEnd(void * pBufferContext) noexcept {}
	void OnLoopEnd(void * pBufferContext) noexcept {}
	void OnVoiceError(void * pBufferContext, HRESULT Error) noexcept {}
};

// #############################################################################
//                           Windows Globals
// #############################################################################
static xAudioVoice voiceArr[MAX_CONCURRENT_SOUNDS];

// #############################################################################
//                           Platform Implementations
// #############################################################################
LRESULT CALLBACK windows_window_callback(HWND window, UINT msg,
                                         WPARAM wParam, LPARAM lParam)
{
  LRESULT result = 0;

  auto * ctx = reinterpret_cast<PlatformContext*>(
      GetWindowLongPtr(window, GWLP_USERDATA));

  // WM_NCCREATE (or WM_CREATE) gives us the CREATESTRUCT with lpCreateParams
  if (msg == WM_NCCREATE)
  {
    const auto* cs = reinterpret_cast<CREATESTRUCTA*>(lParam);
    auto* createCtx = static_cast<PlatformContext*>(cs->lpCreateParams);
    if (createCtx)
    {
      // store it in GWLP_USERDATA for future callbacks
      SetWindowLongPtr(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(createCtx));
      ctx = createCtx;
    }
  }

  if (!ctx) {
    return DefWindowProcA(window, msg, wParam, lParam);
  }

  if (ctx->m_OverlayInputHandler) {
    // You can read the io.WantCaptureMouse, io.WantCaptureKeyboard flags to tell if dear imgui wants to use your inputs.
    // - When io.WantCaptureMouse is true, do not dispatch mouse input data to your main application, or clear/overwrite your copy of the mouse data.
    // - When io.WantCaptureKeyboard is true, do not dispatch keyboard input data to your main application, or clear/overwrite your copy of the keyboard data.
      if (ctx->m_OverlayInputHandler(window, msg, wParam, lParam)) {

      }
  }

  switch(msg)
  {
    case WM_CLOSE:
    {
      ctx->m_Running = false;
      SM_TRACE("WM_CLOSE received, exiting...");
      break;
    }

    case WM_SIZE:
    {
      RECT rect = {0};
      GetClientRect(window, &rect);

      const UINT dpi = GetDpiForWindow(window);
      const int physicalW = MulDiv((rect.right - rect.left), dpi, 96);
      const int physicalH = MulDiv((rect.bottom - rect.top), dpi, 96);

      ctx->m_Input->screenSize.x = static_cast<float>(physicalW);
      ctx->m_Input->screenSize.y = static_cast<float>(physicalH);
      ctx->m_ResizeRequested = true;
      ctx->m_Width = physicalW;
      ctx->m_Height = physicalH;

      SM_TRACE("WM_SIZE, Resizing to %d x %d", ctx->m_Width, ctx->m_Height);

      break;
    }

    case WM_ENTERSIZEMOVE: {
      ctx->m_DraggingWindow = true;
      break;
    }
    case WM_EXITSIZEMOVE: {
      ctx->m_DraggingWindow = false;
      break;
    }

    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
    {
      bool isDown = (msg == WM_KEYDOWN) || (msg == WM_SYSKEYDOWN) ||
                    (msg == WM_LBUTTONDOWN);

      const KeyCodeID keyCode = ctx->m_KeyCodeLookupTable[wParam];
      Key* key = &ctx->m_Input->keys[keyCode];
      key->justPressed = !key->justPressed && !key->isDown && isDown;
      key->justReleased = !key->justReleased && key->isDown && !isDown;
      key->isDown = isDown;
      key->halfTransitionCount++;

      break;
    }

    case WM_LBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_MBUTTONUP:
    case WM_RBUTTONUP:
    {
      bool isDown = (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN);
      int mouseCode = 
        (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP)? VK_LBUTTON: 
        (msg == WM_MBUTTONDOWN || msg == WM_MBUTTONUP)? VK_MBUTTON: VK_RBUTTON;

      const KeyCodeID keyCode = ctx->m_KeyCodeLookupTable[mouseCode];
      Key* key = &ctx->m_Input->keys[keyCode];
      key->justPressed = !key->justPressed && !key->isDown && isDown;
      key->justReleased = !key->justReleased && key->isDown && !isDown;
      key->isDown = isDown;
      key->halfTransitionCount++;

      break;
    }

    default:
    {
      // Let windows handle the default input for now
      result = DefWindowProcA(window, msg, wParam, lParam);
    }
  }

  return result;
}

PlatformContext* platform_init(BumpAllocator* persistentStorage, BumpAllocator* transientStorage) {
  auto* ctx = reinterpret_cast<PlatformContext *>(bump_alloc(persistentStorage, sizeof(PlatformContext)));
  if (!ctx) {
    SM_ERROR("Failed to allocate PlatformContext");
    return nullptr;
  }

  ctx->m_Type = PlatformType::PLATFORM_WINDOWS;
  ctx->m_PersistentStorage = persistentStorage;
  ctx->m_TransientStorage = transientStorage;
  ctx->m_PlatformHandle = nullptr;
  ctx->m_ResizeRequested = false;
  ctx->m_Running = false;
  ctx->m_MusicVolume = 0.25f;
  ctx->m_DraggingWindow = false;

  // Initialize frame stats
  ctx->m_FrameStats = {};
  ctx->m_FrameStats.deltaSeconds = 0.0f;
  ctx->m_FrameStats.frameTimeMs = 0.0f;
  ctx->m_FrameStats.fpsInstant = 0.0f;
  ctx->m_FrameStats.fpsSmoothed = 0.0f;
  ctx->m_FrameStats.elapsedSeconds = 0.0;
  ctx->m_FrameStats.frameCount = 0;

  return ctx;
}

void platform_shutdown(PlatformContext* ctx) {
  if (!ctx) {
    return;
  }

  // Stop and destroy any active XAudio2 source voices
  for (int i = 0; i < MAX_CONCURRENT_SOUNDS; ++i)
  {
    xAudioVoice* v = &voiceArr[i];
    if (v->voice)
    {
      v->voice->Stop();
      v->voice->FlushSourceBuffers();
      v->voice->DestroyVoice();
      v->voice = nullptr;
      v->playing = false;
      v->fadeTimer = 0.0f;
      v->options = 0;
      v->soundPath = nullptr;
    }
  }

  // Destroy the window if it exists
  if (ctx->m_PlatformHandle)
  {
    DestroyWindow(static_cast<HWND>(ctx->m_PlatformHandle));
    ctx->m_PlatformHandle = nullptr;
  }

  ctx->m_Running = false;

  // Match CoInitializeEx in platform_init_audio
  CoUninitialize();
}

bool platform_create_window(PlatformContext* ctx, int width, int height, const char* title, void* windowProps) {
  HINSTANCE hInstance = GetModuleHandleA(nullptr);
  if (windowProps) {
    hInstance = static_cast<HINSTANCE>(windowProps);
  }

  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

  // Setup and register window class
  HICON hIcon = LoadIconA(hInstance, IDI_APPLICATION);
  WNDCLASSA wc = {};
  wc.hInstance = hInstance;
  wc.hIcon = hIcon;
  wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
  wc.lpszClassName = title;
  wc.lpfnWndProc = windows_window_callback;

  if (!RegisterClassA(&wc))
  {
    return false;
  }

  int exStyle = WS_EX_APPWINDOW;

  int dwStyle = WS_OVERLAPPEDWINDOW;


  // Add Border Size of the window
  {
    RECT borderRect = {};
    AdjustWindowRectEx(&borderRect, dwStyle, 0, exStyle);

    width += borderRect.right - borderRect.left;
    height += borderRect.bottom - borderRect.top;
  }

  ctx->m_Width = width;
  ctx->m_Height = height;

  ctx->m_PlatformHandle = CreateWindowExA(
      exStyle,
      wc.lpszClassName,
      title,
      dwStyle,
      CW_USEDEFAULT, CW_USEDEFAULT,
      ctx->m_Width, ctx->m_Height,
      nullptr,
      nullptr,
      hInstance,
      ctx
  );

  if (!ctx->m_PlatformHandle)
  {
    SM_ASSERT(false, "Failed to create window!");
    return false;
  }

  ShowWindow(static_cast<HWND>(ctx->m_PlatformHandle), SW_SHOW);

  ctx->m_Running = true;


  return true;
}

void platform_fill_keycode_lookup_table(PlatformContext* ctx) {
  if (!ctx) {
    SM_ERROR("platform_fill_keycode_lookup_table: ctx is null");
    return;
  }

  ctx->m_KeyCodeLookupTable[VK_LBUTTON] = KEY_MOUSE_LEFT;
  ctx->m_KeyCodeLookupTable[VK_MBUTTON] = KEY_MOUSE_MIDDLE;
  ctx->m_KeyCodeLookupTable[VK_RBUTTON] = KEY_MOUSE_RIGHT;

  ctx->m_KeyCodeLookupTable['A'] = KEY_A;
  ctx->m_KeyCodeLookupTable['B'] = KEY_B;
  ctx->m_KeyCodeLookupTable['C'] = KEY_C;
  ctx->m_KeyCodeLookupTable['D'] = KEY_D;
  ctx->m_KeyCodeLookupTable['E'] = KEY_E;
  ctx->m_KeyCodeLookupTable['F'] = KEY_F;
  ctx->m_KeyCodeLookupTable['G'] = KEY_G;
  ctx->m_KeyCodeLookupTable['H'] = KEY_H;
  ctx->m_KeyCodeLookupTable['I'] = KEY_I;
  ctx->m_KeyCodeLookupTable['J'] = KEY_J;
  ctx->m_KeyCodeLookupTable['K'] = KEY_K;
  ctx->m_KeyCodeLookupTable['L'] = KEY_L;
  ctx->m_KeyCodeLookupTable['M'] = KEY_M;
  ctx->m_KeyCodeLookupTable['N'] = KEY_N;
  ctx->m_KeyCodeLookupTable['O'] = KEY_O;
  ctx->m_KeyCodeLookupTable['P'] = KEY_P;
  ctx->m_KeyCodeLookupTable['Q'] = KEY_Q;
  ctx->m_KeyCodeLookupTable['R'] = KEY_R;
  ctx->m_KeyCodeLookupTable['S'] = KEY_S;
  ctx->m_KeyCodeLookupTable['T'] = KEY_T;
  ctx->m_KeyCodeLookupTable['U'] = KEY_U;
  ctx->m_KeyCodeLookupTable['V'] = KEY_V;
  ctx->m_KeyCodeLookupTable['W'] = KEY_W;
  ctx->m_KeyCodeLookupTable['X'] = KEY_X;
  ctx->m_KeyCodeLookupTable['Y'] = KEY_Y;
  ctx->m_KeyCodeLookupTable['Z'] = KEY_Z;
  ctx->m_KeyCodeLookupTable['0'] = KEY_0;
  ctx->m_KeyCodeLookupTable['1'] = KEY_1;
  ctx->m_KeyCodeLookupTable['2'] = KEY_2;
  ctx->m_KeyCodeLookupTable['3'] = KEY_3;
  ctx->m_KeyCodeLookupTable['4'] = KEY_4;
  ctx->m_KeyCodeLookupTable['5'] = KEY_5;
  ctx->m_KeyCodeLookupTable['6'] = KEY_6;
  ctx->m_KeyCodeLookupTable['7'] = KEY_7;
  ctx->m_KeyCodeLookupTable['8'] = KEY_8;
  ctx->m_KeyCodeLookupTable['9'] = KEY_9;

  ctx->m_KeyCodeLookupTable[VK_SPACE] = KEY_SPACE,
  ctx->m_KeyCodeLookupTable[VK_OEM_3] = KEY_TICK,
  ctx->m_KeyCodeLookupTable[VK_OEM_MINUS] = KEY_MINUS,
  // TODO ???
  ctx->m_KeyCodeLookupTable[VK_OEM_PLUS] = KEY_EQUAL,
  ctx->m_KeyCodeLookupTable[VK_OEM_4] = KEY_LEFT_BRACKET,
  ctx->m_KeyCodeLookupTable[VK_OEM_6] = KEY_RIGHT_BRACKET,
  ctx->m_KeyCodeLookupTable[VK_OEM_1] = KEY_SEMICOLON,
  ctx->m_KeyCodeLookupTable[VK_OEM_7] = KEY_QUOTE,
  ctx->m_KeyCodeLookupTable[VK_OEM_COMMA] = KEY_COMMA,
  ctx->m_KeyCodeLookupTable[VK_OEM_PERIOD] = KEY_PERIOD,
  ctx->m_KeyCodeLookupTable[VK_OEM_2] = KEY_FORWARD_SLASH,
  ctx->m_KeyCodeLookupTable[VK_OEM_5] = KEY_BACKWARD_SLASH,
  ctx->m_KeyCodeLookupTable[VK_TAB] = KEY_TAB,
  ctx->m_KeyCodeLookupTable[VK_ESCAPE] = KEY_ESCAPE,
  ctx->m_KeyCodeLookupTable[VK_PAUSE] = KEY_PAUSE,
  ctx->m_KeyCodeLookupTable[VK_UP] = KEY_UP,
  ctx->m_KeyCodeLookupTable[VK_DOWN] = KEY_DOWN,
  ctx->m_KeyCodeLookupTable[VK_LEFT] = KEY_LEFT,
  ctx->m_KeyCodeLookupTable[VK_RIGHT] = KEY_RIGHT,
  ctx->m_KeyCodeLookupTable[VK_BACK] = KEY_BACKSPACE,
  ctx->m_KeyCodeLookupTable[VK_RETURN] = KEY_RETURN,
  ctx->m_KeyCodeLookupTable[VK_DELETE] = KEY_DELETE,
  ctx->m_KeyCodeLookupTable[VK_INSERT] = KEY_INSERT,
  ctx->m_KeyCodeLookupTable[VK_HOME] = KEY_HOME,
  ctx->m_KeyCodeLookupTable[VK_END] = KEY_END,
  ctx->m_KeyCodeLookupTable[VK_PRIOR] = KEY_PAGE_UP,
  ctx->m_KeyCodeLookupTable[VK_NEXT] = KEY_PAGE_DOWN,
  ctx->m_KeyCodeLookupTable[VK_CAPITAL] = KEY_CAPS_LOCK,
  ctx->m_KeyCodeLookupTable[VK_NUMLOCK] = KEY_NUM_LOCK,
  ctx->m_KeyCodeLookupTable[VK_SCROLL] = KEY_SCROLL_LOCK,
  ctx->m_KeyCodeLookupTable[VK_APPS] = KEY_MENU,

  ctx->m_KeyCodeLookupTable[VK_SHIFT] = KEY_SHIFT,
  ctx->m_KeyCodeLookupTable[VK_LSHIFT] = KEY_SHIFT,
  ctx->m_KeyCodeLookupTable[VK_RSHIFT] = KEY_SHIFT,

  ctx->m_KeyCodeLookupTable[VK_CONTROL] = KEY_CONTROL,
  ctx->m_KeyCodeLookupTable[VK_LCONTROL] = KEY_CONTROL,
  ctx->m_KeyCodeLookupTable[VK_RCONTROL] = KEY_CONTROL,

  ctx->m_KeyCodeLookupTable[VK_MENU] = KEY_ALT,
  ctx->m_KeyCodeLookupTable[VK_LMENU] = KEY_ALT,
  ctx->m_KeyCodeLookupTable[VK_RMENU] = KEY_ALT,

  ctx->m_KeyCodeLookupTable[VK_F1] = KEY_F1;
  ctx->m_KeyCodeLookupTable[VK_F2] = KEY_F2;
  ctx->m_KeyCodeLookupTable[VK_F3] = KEY_F3;
  ctx->m_KeyCodeLookupTable[VK_F4] = KEY_F4;
  ctx->m_KeyCodeLookupTable[VK_F5] = KEY_F5;
  ctx->m_KeyCodeLookupTable[VK_F6] = KEY_F6;
  ctx->m_KeyCodeLookupTable[VK_F7] = KEY_F7;
  ctx->m_KeyCodeLookupTable[VK_F8] = KEY_F8;
  ctx->m_KeyCodeLookupTable[VK_F9] = KEY_F9;
  ctx->m_KeyCodeLookupTable[VK_F10] = KEY_F10;
  ctx->m_KeyCodeLookupTable[VK_F11] = KEY_F11;
  ctx->m_KeyCodeLookupTable[VK_F12] = KEY_F12;

  ctx->m_KeyCodeLookupTable[VK_NUMPAD0] = KEY_NUMPAD_0;
  ctx->m_KeyCodeLookupTable[VK_NUMPAD1] = KEY_NUMPAD_1;
  ctx->m_KeyCodeLookupTable[VK_NUMPAD2] = KEY_NUMPAD_2;
  ctx->m_KeyCodeLookupTable[VK_NUMPAD3] = KEY_NUMPAD_3;
  ctx->m_KeyCodeLookupTable[VK_NUMPAD4] = KEY_NUMPAD_4;
  ctx->m_KeyCodeLookupTable[VK_NUMPAD5] = KEY_NUMPAD_5;
  ctx->m_KeyCodeLookupTable[VK_NUMPAD6] = KEY_NUMPAD_6;
  ctx->m_KeyCodeLookupTable[VK_NUMPAD7] = KEY_NUMPAD_7;
  ctx->m_KeyCodeLookupTable[VK_NUMPAD8] = KEY_NUMPAD_8;
  ctx->m_KeyCodeLookupTable[VK_NUMPAD9] = KEY_NUMPAD_9;
}

void platform_update_window(PlatformContext* ctx) {
  // Gather new Input
  MSG msg;
  while(PeekMessageA(&msg, static_cast<HWND>(ctx->m_PlatformHandle), 0, 0, PM_REMOVE))
  {
    TranslateMessage(&msg);
    DispatchMessageA(&msg); // Calls the callback specified when creating the window
  }

  // Mouse Position
  {
    POINT point = {};
    GetCursorPos(&point);
    ScreenToClient(static_cast<HWND>(ctx->m_PlatformHandle), &point);

    // If the OS is dragging/resizing the window, swallow deltas to avoid spikes
    if (ctx->m_DraggingWindow) {
      ctx->m_Input->relMouse = {0, 0};
      ctx->m_Input->mousePos = {point.x, point.y};
      ctx->m_Input->prevMousePos = ctx->m_Input->mousePos;

      glm::ivec2 w = screen_to_world(ctx->m_Input, ctx->m_RenderData, ctx->m_Input->mousePos);
      glm::vec3 worldPos = { static_cast<float>(w.x), static_cast<float>(w.y), 0.0f };
      ctx->m_Input->relMouseWorld = {0.0f, 0.0f, 0.0f};
      ctx->m_Input->mouseWorldPos = worldPos;
      ctx->m_Input->prevMouseWorldPos = worldPos;
    } else {
      // Compute relative mouse movement (screen space)
      ctx->m_Input->relMouse.x = point.x - ctx->m_Input->prevMousePos.x;
      ctx->m_Input->relMouse.y = point.y - ctx->m_Input->prevMousePos.y;

      // Update current and previous mouse positions (screen space)
      ctx->m_Input->mousePos.x = point.x;
      ctx->m_Input->mousePos.y = point.y;
      ctx->m_Input->prevMousePos = ctx->m_Input->mousePos;

      // Mouse Position World
      {
        glm::ivec2 w = screen_to_world(ctx->m_Input, ctx->m_RenderData, ctx->m_Input->mousePos);
        glm::vec3 worldPos = { static_cast<float>(w.x), static_cast<float>(w.y), 0.0f };

        // Compute relative mouse movement (world space)
        ctx->m_Input->relMouseWorld = worldPos - ctx->m_Input->prevMouseWorldPos;

        // Update current and previous mouse positions (world space)
        ctx->m_Input->mouseWorldPos = worldPos;
        ctx->m_Input->prevMouseWorldPos = worldPos;
      }
    }
  }
}

void* platform_load_dynamic_library(const char* dll) {
  const HMODULE result = LoadLibraryA(dll);
  SM_ASSERT(result, "Failed to load dll: %s", dll);

  return result;
}

void* platform_load_dynamic_function(void* dll, const char* funName) {
  FARPROC proc = GetProcAddress(static_cast<HMODULE>(dll), funName);
  SM_ASSERT(proc, "Failed to load function: %s from DLL", funName);

  return static_cast<void *>(proc);
}

bool platform_free_dynamic_library(void* dll) {
  SM_ASSERT(dll, "No DLL supplied!");
  BOOL freeResult = FreeLibrary(static_cast<HMODULE>(dll));
  SM_ASSERT(freeResult, "Failed to FreeLibrary");

  return static_cast<bool>(freeResult);
}

// #############################
// File system watcher (Windows)
// #############################
struct FileWatchHandle {
  HANDLE changeHandle;
  HANDLE stopEvent;
  HANDLE threadHandle;
  char   directory[MAX_PATH];
  char   filename[MAX_PATH];
  char   fullpath[MAX_PATH];
  FILETIME lastWriteTime;
  std::atomic<bool> changed;
  DWORD  debounceMs;
  // Coalescing window to suppress multiple signals from a single build/link
  ULONGLONG lastSignalTick;
  DWORD  coalesceMs;
};

static void split_path_file(const char* path, char* outDir, size_t outDirSize, char* outFile, size_t outFileSize)
{
  const char* lastSlash = strrchr(path, '\\');
  const char* lastFwd   = strrchr(path, '/');
  const char* sep = lastSlash ? lastSlash : lastFwd;
  if (sep) {
    size_t dirLen = static_cast<size_t>(sep - path);
    if (dirLen >= outDirSize) dirLen = outDirSize - 1;
    memcpy(outDir, path, dirLen);
    outDir[dirLen] = '\0';

    const char* fileStart = sep + 1;
    strncpy_s(outFile, outFileSize, fileStart, _TRUNCATE);
  } else {
    // No directory component; use current directory
    strncpy_s(outDir, outDirSize, ".", _TRUNCATE);
    strncpy_s(outFile, outFileSize, path, _TRUNCATE);
  }
}

static bool get_file_last_write_timeA(const char* path, FILETIME* out)
{
  WIN32_FILE_ATTRIBUTE_DATA fad{};
  if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fad))
    return false;
  *out = fad.ftLastWriteTime;
  return true;
}

static DWORD WINAPI file_watch_thread_proc(LPVOID param)
{
  auto* h = reinterpret_cast<FileWatchHandle*>(param);
  HANDLE waitHandles[2] = { h->stopEvent, h->changeHandle };

  for (;;) {
    DWORD w = WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);
    if (w == WAIT_OBJECT_0) {
      // stopEvent signaled
      break;
    }
    if (w == WAIT_OBJECT_0 + 1) {
      // Directory changed: first rearm the change notification so we can wait for a quiet period
      if (!FindNextChangeNotification(h->changeHandle)) {
        break;
      }

      // Snapshot current file time (may fail if file not present yet)
      FILETIME snapshot{};
      bool snapshotValid = get_file_last_write_timeA(h->fullpath, &snapshot);

      // Debounce loop: wait for no further directory changes for debounceMs
      for (;;) {
        DWORD wq = WaitForMultipleObjects(2, waitHandles, FALSE, h->debounceMs);
        if (wq == WAIT_OBJECT_0) {
          // stop requested
          return 0;
        }
        if (wq == WAIT_TIMEOUT) {
          // Quiet period elapsed: verify file timestamp stability
          FILETIME nowFT{};
          bool nowValid = get_file_last_write_timeA(h->fullpath, &nowFT);
          if (!nowValid) {
            // File missing; keep debouncing until it reappears
            snapshotValid = false;
            continue;
          }

          // If we didn't have a valid snapshot, treat current as the first stable observation
          if (!snapshotValid) {
            // Only signal if this differs from the previously recorded timestamp
            if (CompareFileTime(&nowFT, &h->lastWriteTime) != 0) {
              // Coalesce multiple notifications occurring shortly after the first stable change
              ULONGLONG tick = GetTickCount64();
              if (h->lastSignalTick == 0 || (tick - h->lastSignalTick) >= h->coalesceMs) {
                h->lastSignalTick = tick;
                h->lastWriteTime = nowFT;
                h->changed.store(true, std::memory_order_release);
              } else {
                // Suppress extra event within coalescing window, but still update lastWriteTime
                h->lastWriteTime = nowFT;
              }
            }
            break; // either way, exit debounce loop; watcher is re-armed already
          }

          // We had a snapshot and it's equal after quiet window -> stable
          if (CompareFileTime(&nowFT, &snapshot) == 0) {
            // Only signal if this stable time is actually different from what we last reported
            if (CompareFileTime(&nowFT, &h->lastWriteTime) != 0) {
              // Coalesce multiple notifications occurring shortly after the first stable change
              ULONGLONG tick = GetTickCount64();
              if (h->lastSignalTick == 0 || (tick - h->lastSignalTick) >= h->coalesceMs) {
                h->lastSignalTick = tick;
                h->lastWriteTime = nowFT;
                h->changed.store(true, std::memory_order_release);
              } else {
                // Suppress extra event within coalescing window, but still update lastWriteTime
                h->lastWriteTime = nowFT;
              }
            }
            break; // done with this change notification
          } else {
            // Timestamp still changing; update snapshot and wait another quiet window
            snapshot = nowFT;
            snapshotValid = true;
            continue;
          }
        }
        if (wq == WAIT_OBJECT_0 + 1) {
          // Another directory change arrived; rearm and continue waiting
          if (!FindNextChangeNotification(h->changeHandle)) {
            return 0;
          }
          // refresh snapshot to the latest write time if available
          FILETIME tmp{};
          bool got = get_file_last_write_timeA(h->fullpath, &tmp);
          if (got) {
            snapshot = tmp;
          }
          snapshotValid = got;
          continue;
        }
      }

      // Already re-armed inside the loop; ready for next notifications
    }
  }

  return 0;
}

FileWatchHandle* platform_watch_file(const char* file_path, BumpAllocator* allocator)
{
  if (!file_path) return nullptr;

  void* memBlock = bump_alloc(allocator, sizeof(FileWatchHandle));
  if (!memBlock) return nullptr;
  auto* h = new (memBlock) FileWatchHandle();
  memset(h, 0, sizeof(FileWatchHandle));
  h->changed.store(false, std::memory_order_relaxed);
  h->debounceMs = 250; // default debounce for stabilization window
  h->lastSignalTick = 0; // no previous signals
  h->coalesceMs = 2000;  // suppress back-to-back signals within this window

  // Fill paths
  strncpy_s(h->fullpath, file_path, _TRUNCATE);
  split_path_file(file_path, h->directory, sizeof(h->directory), h->filename, sizeof(h->filename));

  // Prime last write time (ok if it fails)
  get_file_last_write_timeA(h->fullpath, &h->lastWriteTime);

  h->stopEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  if (!h->stopEvent) { delete h; return nullptr; }

  // Watch directory for relevant changes
  const DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION;
  h->changeHandle = FindFirstChangeNotificationA(h->directory, FALSE, filter);
  if (h->changeHandle == INVALID_HANDLE_VALUE) {
    CloseHandle(h->stopEvent);
    return nullptr;
  }

  // Start background thread
  h->threadHandle = CreateThread(nullptr, 0, &file_watch_thread_proc, h, 0, nullptr);
  if (!h->threadHandle) {
    FindCloseChangeNotification(h->changeHandle);
    CloseHandle(h->stopEvent);
    return nullptr;
  }

  return h;
}

bool platform_file_changed(FileWatchHandle* handle)
{
  if (!handle) return false;
  return handle->changed.exchange(false, std::memory_order_acquire);
}

void platform_unwatch_file(FileWatchHandle* handle)
{
  if (!handle) return;
  // Signal stop and wait
  if (handle->stopEvent) {
    SetEvent(handle->stopEvent);
  }
  if (handle->threadHandle) {
    WaitForSingleObject(handle->threadHandle, 5000);
    CloseHandle(handle->threadHandle);
  }
  if (handle->changeHandle && handle->changeHandle != INVALID_HANDLE_VALUE) {
    FindCloseChangeNotification(handle->changeHandle);
  }
  if (handle->stopEvent) {
    CloseHandle(handle->stopEvent);
  }
}

bool platform_init_audio(PlatformContext* ctx) {
  HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if(FAILED(hr)) { return false; }

  IXAudio2* xaudio2 = nullptr;
  hr = XAudio2Create(&xaudio2, 0, XAUDIO2_DEFAULT_PROCESSOR);
  if(FAILED(hr)) { return false; }

  IXAudio2MasteringVoice* master_voice = nullptr;
  hr = xaudio2->CreateMasteringVoice(&master_voice);
  if(FAILED(hr)) { return false; }

  WAVEFORMATEX wave = {};
  wave.wFormatTag = WAVE_FORMAT_PCM;
  wave.nChannels = NUM_CHANNELS;
  wave.nSamplesPerSec = SAMPLE_RATE;
  wave.wBitsPerSample = 16;
  wave.nBlockAlign = (NUM_CHANNELS * wave.wBitsPerSample) / 8;
  wave.nAvgBytesPerSec = SAMPLE_RATE * wave.nBlockAlign;

  for(int voiceIdx = 0; voiceIdx < MAX_CONCURRENT_SOUNDS; voiceIdx++)
  {
    xAudioVoice* voice = &voiceArr[voiceIdx];
    hr = xaudio2->CreateSourceVoice(&voice->voice, &wave, 0, XAUDIO2_DEFAULT_FREQ_RATIO, voice, nullptr, nullptr);
    voice->voice->SetVolume(ctx->m_MusicVolume);
    if(FAILED(hr)) { return false; }
  }

  return true;
}

void platform_update_audio(PlatformContext* ctx, float dt) {
  for(int soundIdx = 0; soundIdx < g_SoundState->playingSounds.count; soundIdx++)
  {
    Sound* sound = &g_SoundState->playingSounds[soundIdx];

    // Playing Sounds
    if(sound->options & SOUND_OPTION_START ||
       sound->options & SOUND_OPTION_FADE_IN)
    {
      SM_ASSERT(sound->size > 0, "Sound has no Samples Size: %d",
                                    sound->size);
      SM_ASSERT(sound->data, "Sound has no Data!");
      sound->options = 0;

      xAudioVoice* voice = nullptr;
      for(int voiceIdx = 0; voiceIdx < MAX_CONCURRENT_SOUNDS; voiceIdx++)
      {
        xAudioVoice* possibleVoice = &voiceArr[voiceIdx];
        if(!possibleVoice->playing)
        {
          voice = possibleVoice;
          break;
        }
      }

      if(voice != nullptr)
      {
        XAUDIO2_BUFFER buffer = {};
        buffer.Flags = XAUDIO2_END_OF_STREAM;
        buffer.AudioBytes = sound->size;
        buffer.pAudioData = (BYTE*)sound->data;

        HRESULT hr = voice->voice->SubmitSourceBuffer(&buffer);
        if(!FAILED(hr))
        {
          voice->voice->Start();
          voice->soundPath = sound->path;
		      InterlockedExchange((LONG*)&voice->playing, true);
        }
      }
    }

    // Stopping Sounds
    if(sound->options & SOUND_OPTION_FADE_OUT)
    {
      xAudioVoice* voice = nullptr;
      for(int voiceIdx = 0; voiceIdx < MAX_CONCURRENT_SOUNDS; voiceIdx++)
      {
        xAudioVoice* possibleVoice = &voiceArr[voiceIdx];
        if(!possibleVoice->playing)
        {
          continue;
        }

        if(strcmp(possibleVoice->soundPath, sound->path) == 0)
        {
          possibleVoice->options = SOUND_OPTION_FADE_OUT;
        }
      }
    }
  }

  // Update Voices
  for(int voiceIdx = 0; voiceIdx < MAX_CONCURRENT_SOUNDS; voiceIdx++)
  {
    xAudioVoice* voice = &voiceArr[voiceIdx];

    if(voice->options & SOUND_OPTION_FADE_IN)
    {
      voice->fadeTimer = min(voice->fadeTimer + dt, FADE_DURATION);
      float t = voice->fadeTimer / FADE_DURATION;
      voice->voice->SetVolume(t * ctx->m_MusicVolume);

      if(voice->fadeTimer == FADE_DURATION)
      {
        voice->options ^= SOUND_OPTION_FADE_IN;
        voice->fadeTimer = 0.0f;
      }

      // If some clown sets both options, SOUND_OPTION_FADE_IN will start
      // then SOUND_OPTION_FADE_OUT will happen
      continue;
    }

    if(voice->options & SOUND_OPTION_FADE_OUT)
    {
      voice->fadeTimer = min(voice->fadeTimer + dt, FADE_DURATION);
      float t = 1.0f - voice->fadeTimer / FADE_DURATION;
      voice->voice->SetVolume(t * ctx->m_MusicVolume);

      if(voice->fadeTimer == FADE_DURATION)
      {
        voice->options ^= SOUND_OPTION_FADE_OUT;
        voice->voice->Stop();
        voice->voice->FlushSourceBuffers(); // Remove the buffer from the voice
        voice->voice->SetVolume(1.0f); // Reset Volume
        voice->fadeTimer = 0.0f;
      }
    }
  }

  g_SoundState->playingSounds.count = 0;
}

void platform_sleep(const unsigned int ms) {
  ::Sleep(ms);
}

bool platform_is_minimized(PlatformContext* ctx)
{
  if (!ctx || !ctx->m_PlatformHandle) return false;
  return IsIconic(static_cast<HWND>(ctx->m_PlatformHandle));
}

// Platform-specific assertion handler for Windows.
// Shows a message box with details and then breaks into the debugger.
void platform_debug_break(const char* expr, const char* file, int line, const char* message)
{
  char buffer[2048] = {};
  // Compose a detailed message
  sprintf_s(buffer,
          "Assertion failed!\n\nExpression: %s\nFile: %s\nLine: %d\n\n%s",
          (expr ? expr : "<none>"),
          (file ? file : "<unknown>"),
          line,
          (message ? message : "<no message>"));

  MessageBoxA(nullptr, buffer, "Assertion Failed", MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST);

  // Break into the debugger (still platform-defined macro)
  DEBUG_BREAK();
}

