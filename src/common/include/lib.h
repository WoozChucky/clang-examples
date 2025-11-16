#pragma once

// Used to open Files
#include <cstdio>

// Used to get malloc
#include <cstdlib>

#include <cmath>

// Used to get the timestamp of a file
#include <sys/stat.h>

// Used to get memset
#include <cstring>

#include <print>
#include <ostream>

enum class RendererAPI : uint8_t {
    Invalid,
    DirectX12,
    DirectX11,
    Vulkan,
};

// #############################################################################
//                           Constants
// #############################################################################
// WAV Files
static constexpr int NUM_CHANNELS = 2;
static constexpr int SAMPLE_RATE = 44100;

// #############################################################################
//                           Defines
// #############################################################################
#define BIT(x) 1 << (x)
#define KB(x) ((unsigned long long)1024 * x)
#define MB(x) ((unsigned long long)1024 * KB(x))
#define GB(x) ((unsigned long long)1024 * MB(x))

#define line_id(index) (size_t)((__LINE__ << 16) | (index))

#define ArraySize(x) (sizeof((x)) / sizeof((x)[0]))

#ifdef _WIN32
#define DEBUG_BREAK() __debugbreak()
#define EXPORT_FN __declspec(dllexport)
#elif __linux__
#define DEBUG_BREAK() __builtin_debugtrap()
#define EXPORT_FN
#elif __APPLE__
#define DEBUG_BREAK() __builtin_trap()
#define EXPORT_FN
#endif

#define BEGIN_TIMED_BLOCK(ID) uint64_t StartCycleCount##ID = __rdtsc();
#define END_TIMED_BLOCK(ID, OUT_VAR) \
  OUT_VAR = __rdtsc() - StartCycleCount##ID;

#if 0
struct DebugCycleCounter
{
  uint64_t startCycleCount = 0;
  uint64_t& outCycleCount;

  DebugCycleCounter(uint64_t& outCycleCountRef)
    : outCycleCount(outCycleCountRef)
  {
    startCycleCount = __rdtsc();
  }

  ~DebugCycleCounter()
  {
    outCycleCount = __rdtsc() - startCycleCount;
  }
};
#endif

// #############################################################################
//                           Logging
// #############################################################################
// Aparently works on Windows, Linux and Mac
enum TextColor
{
  TEXT_COLOR_BLACK,
  TEXT_COLOR_RED,
  TEXT_COLOR_GREEN,
  TEXT_COLOR_YELLOW,
  TEXT_COLOR_BLUE,
  TEXT_COLOR_MAGENTA,
  TEXT_COLOR_CYAN,
  TEXT_COLOR_WHITE,
  TEXT_COLOR_BRIGHT_BLACK,
  TEXT_COLOR_BRIGHT_RED,
  TEXT_COLOR_BRIGHT_GREEN,
  TEXT_COLOR_BRIGHT_YELLOW,
  TEXT_COLOR_BRIGHT_BLUE,
  TEXT_COLOR_BRIGHT_MAGENTA,
  TEXT_COLOR_BRIGHT_CYAN,
  TEXT_COLOR_BRIGHT_WHITE,
  TEXT_COLOR_COUNT
};

template <typename... Args>
void _log(const char* prefix, const char* msg, TextColor textColor, Args... args)
{
  static const char* TextColorTable [TEXT_COLOR_COUNT] =
  {
    "\x1b[30m", // TEXT_COLOR_BLACK
    "\x1b[31m", // TEXT_COLOR_RED
    "\x1b[32m", // TEXT_COLOR_GREEN
    "\x1b[33m", // TEXT_COLOR_YELLOW
    "\x1b[34m", // TEXT_COLOR_BLUE
    "\x1b[35m", // TEXT_COLOR_MAGENTA
    "\x1b[36m", // TEXT_COLOR_CYAN
    "\x1b[37m", // TEXT_COLOR_WHITE
    "\x1b[90m", // TEXT_COLOR_BRIGHT_BLACK
    "\x1b[91m", // TEXT_COLOR_BRIGHT_RED
    "\x1b[92m", // TEXT_COLOR_BRIGHT_GREEN
    "\x1b[93m", // TEXT_COLOR_BRIGHT_YELLOW
    "\x1b[94m", // TEXT_COLOR_BRIGHT_BLUE
    "\x1b[95m", // TEXT_COLOR_BRIGHT_MAGENTA
    "\x1b[96m", // TEXT_COLOR_BRIGHT_CYAN
    "\x1b[97m", // TEXT_COLOR_BRIGHT_WHITE
  };

  char formatBuffer[8192] = {};
  sprintf(formatBuffer, "%s %s %s \033[0m",TextColorTable[textColor], prefix, msg);

  static char buffer[8192] = {};
  sprintf(buffer, formatBuffer, args...);
  std::println("{}", buffer);
}

#define SM_TRACE(msg, ...) _log("TRACE:", msg, TEXT_COLOR_GREEN, ##__VA_ARGS__);
#define SM_WARN(msg, ...) _log("WARN:", msg, TEXT_COLOR_YELLOW, "\033[0m", ##__VA_ARGS__);
#define SM_ERROR(msg, ...) _log("ERROR:", msg, TEXT_COLOR_RED, "\033[0m", ##__VA_ARGS__);

// Forward-declare a platform-specific debug break that can show dialogs, etc.
void platform_debug_break(const char* expr, const char* file, int line, const char* message);

// Helper to format, log, and delegate to the platform for assertion handling
template <typename... Args>
inline void sm_assert_fail(const char* expr, const char* file, int line, const char* fmt, Args... args)
{
  // Format the message once for both logging and platform handler
  char formatted[1024] = {};
  sprintf(formatted, fmt, args...);
  // Log through our normal logger
  _log("ERROR:", "%s", TEXT_COLOR_RED, "\033[0m", formatted);
  // Delegate to platform to decide how to break/notify
  platform_debug_break(expr, file, line, formatted);
}

#define SM_ASSERT(x, msg, ...)                   \
{                                                \
  if(!(x))                                       \
  {                                              \
    sm_assert_fail(#x, __FILE__, __LINE__, msg, ##__VA_ARGS__); \
  }                                              \
}

// #############################################################################
//                           Array
// #############################################################################
template<typename T, int N>
struct Array
{
  static constexpr int maxElements = N;
  int count = 0;
  T elements[N];

  T& operator[](int idx)
  {
    SM_ASSERT(idx >= 0, "idx negative!");
    SM_ASSERT(idx < count, "Idx out of bounds!");
    return elements[idx];
  }

  int add(T element)
  {
     SM_ASSERT(count < maxElements, "Array Full!");
    elements[count] = element;
    return count++;
  }

  void remove_idx_and_swap(int idx)
  {
    SM_ASSERT(idx >= 0, "idx negative!");
    SM_ASSERT(idx < count, "idx out of bounds!");
    elements[idx] = elements[--count];
  }

  void clear()
  {
    count = 0;
  }

  [[nodiscard]] bool is_full() const {
    return count == N;
  }
};

// #############################################################################
//                           Math stuff
// #############################################################################
inline float min(float a, float b)
{
  return (a < b)? a : b;
}

inline float max(float a, float b)
{
  return (a > b)? a : b;
}

inline int min(int a, int b)
{
  return (a < b)? a : b;
}

inline int max(int a, int b)
{
  return (a > b)? a : b;
}

inline float clamp(float x, float min, float max)
{
  if(x < min)
  {
    return min;
  }

  if(x > max)
  {
    return max;
  }

  return x;
}

inline int clamp(int x, int min, int max)
{
  if(x < min)
  {
    return min;
  }

  if(x > max)
  {
    return max;
  }

  return x;
}

// speed = 10
// targetSpeed = 100
// speedUP = 12
// speed < targetSpeed? -> speed += speedUp


// speed 200
// targetSpeed = 100
// speedUp = 12
// speed > targetSpeed? -> speed -= speedUp

// speed 100
// targetSpeed = -5
// speedUp = 12
// speed > targetSpeed? -> speed -= speedUp

inline float approach(float current, float target, float increase)
{
  if(current < target)
  {
    return min(current + increase, target);
  }
  return max(current - increase, target);
}

inline int sign(int x)
{
  return (x >= 0)? 1 : -1;
}

inline float sign(float x)
{
  return (x >= 0.0f)? 1.0f : -1.0f;
}

inline float lerp(float a, float b, float t)
{
  return a + (b - a) * t;
}

// #############################################################################
//                           Easing Functions
// #############################################################################
inline float ease_out_linear(float t)
{
  if(t < 1.0f)
  {
    return t;
  }
  else
  {
    return 1.0f;
  }
}

inline float ease_in_quad(float t)
{
  if (t < 1.0f)
  {
    return t * t;
  }
  else
  {
    return 1.0f;
  }
}

inline float ease_out_quad(float t)
{
  if (t < 1.0f)
  {
    return 1.0f - (1.0f - t) * (1.0f - t);
  }
  else
  {
    return 1.0f;
  }
}

inline float ease_in_qubic(float t)
{
  if (t < 1.0f)
  {
    return t * t * t * t;
  }
  else
  {
    return 1.0f;
  }
}

inline float ease_out_qubic(float t)
{
  if (t < 1.0f)
  {
    return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t) * (1.0f - t);
  }
  else
  {
    return 1.0f;
  }
}

inline float ease_in_out_qubic(float t)
{
  if (t < 1.0f)
  {
    return t < 0.5f ? 4.0f * t * t * t : 1 - (float)pow(-2 * t + 2, 3) / 2.0f;
  }
  else
  {
    return 1.0f;
  }
}

inline float ease_wind_slash(float t)
{
  if (t < 1.0f)
  {
    return 1.0f - (float)pow(-2 * (t) + 2, 5) / 33.0f;
  }
  else
  {
    return 1.0f;
  }
}

inline float ease_arrow(float t)
{
  if (t < 1.0f)
  {
    return t <= 0.3f ? 16.0f * t * t * t : 1 - (float)pow(-2 * (t + 0.111) + 2, 5) / 4.0f;
  }
  else
  {
    return 1.0f;
  }
}

inline float ease_in_expo(float t)
{
  if (t < 1.0f)
  {
    return (float)pow(2, 8 * t - 8);
  }
  else
  {
    return 1.0f;
  }
}

inline float ease_out_expo(float t)
{
  if (t < 1.0f)
  {
    return 1.0f - (float)pow(2, -10 * t);
  }
  else
  {
    return 1.0f;
  }
}

inline float ease_out_quint(float t)
{
  if(t < 1.0f)
  {
    return 1.0f - pow(1.0f - t, 5.0f);
  }
  else
  {
    return 1.0f;
  }
}

inline float ease_in_circ(float t)
{
  if (t < 1.0f)
  {
    return 1.0f - sqrt(1 - t * t);
  }
  else
  {
    return 1.0f;
  }
}

inline float ease_out_elastic(float t)
{
  float c4 = (2.0f * 3.14f) / 3.0f;

  if (t == 0.0f)
  {
    return 0.0f;
  }
  else if (t < 1.0f)
  {
    return (float)pow(2, -10 * t) * sinf((t * 10.0f - 0.75f) * c4) + 1.0f;
  }
  else
  {
    return 1.0f;
  }
}

inline float ease_out_back(float t)
{
  float c1 = 1.70158f;
  float c3 = c1 + 1.0f;
  if (t < 1.0f)
  {
    return 1.0f + c3 * powf(t - 1.0f, 3.0f) + c1 * powf(t - 1.0f, 2.0f);
  }
  else
  {
    return 1.0f;
  }
}

inline float superku_function(float t)
{
  if(t > 1.0f)
  {
    return 1.0f;
  }

  return 0.5f * (sqrt(t) + t * t * t * t *t );
}

// #############################################################################
//                           Memory Management
// #############################################################################
struct BumpAllocator
{
  size_t capacity;
  size_t used;
  char* memory;
};

inline BumpAllocator make_bump_allocator(size_t size)
{
  BumpAllocator result = {};

  const size_t alignedSize = size + 7 & ~7;
  result.capacity = alignedSize;
  result.memory = static_cast<char *>(malloc(alignedSize));

  if(result.memory)
  {
    memset(result.memory, 0, alignedSize);
  }
  else
  {
    SM_ASSERT(0, "Failed to malloc memory: %d", size);
  }

  return result;
}

inline char* bump_alloc(BumpAllocator* allocator, size_t size)
{
  char* result = nullptr;

  size_t alignedSize = (size + 7) & ~7;
  if(allocator->used + alignedSize <= allocator->capacity)
  {
    result = allocator->memory + allocator->used;
    allocator->used += alignedSize;
  }
  else
  {
    SM_ASSERT(0, "Bump allocator is full");
  }

  return result;
}

// #############################################################################
//                           String Stuff
// #############################################################################
template <typename... Args>
char* format_text(char* format, Args... args)
{
  static int bufferIdx = 0;
  static char buffers[2][1024] = {};

  char* buffer = buffers[bufferIdx];
  memset(buffer, 0, 1024);

  sprintf(buffer, format, args...);

  return buffer;
}

// #############################################################################
//                           File I/O
// #############################################################################
inline long long get_timestamp(const char* file)
{
    struct stat file_stat = {};
    stat(file, &file_stat);
    return file_stat.st_mtime;
}

inline bool file_exists(char* filePath)
{
  SM_ASSERT(filePath, "No filePath supplied!");

  auto file = fopen(filePath, "rb");
  if(!file)
  {
    return false;
  }
  fclose(file);

  return true;
}

inline long get_file_size(const char* filePath)
{
  SM_ASSERT(filePath, "No filePath supplied!");

  long fileSize = 0;
  auto file = fopen(filePath, "rb");
  if(!file)
  {
    SM_ERROR("Failed opening File (to get_file_size): %s", filePath);
    return 0;
  }

  fseek(file, 0, SEEK_END);
  fileSize = ftell(file);
  fseek(file, 0, SEEK_SET);
  fclose(file);

  return fileSize;
}

/*
* Reads a file into a supplied buffer. We manage our own
* memory and therefore want more control over where it
* is allocated
*/
inline char* read_file(const char* filePath, int* fileSize, char* buffer)
{
  SM_ASSERT(filePath, "No filePath supplied!");
  SM_ASSERT(fileSize, "No fileSize supplied!");

  *fileSize = 0;
  auto file = fopen(filePath, "rb");
  if(!file)
  {
    SM_ERROR("Failed opening File: %s", filePath);
    return nullptr;
  }

  fseek(file, 0, SEEK_END);
  *fileSize = ftell(file);
  fseek(file, 0, SEEK_SET);

  memset(buffer, 0, *fileSize + 1);
  fread(buffer, sizeof(char), *fileSize, file);

  fclose(file);

  return buffer;
}

inline void write_file(char* filePath, char* buffer, int size)
{
  SM_ASSERT(filePath, "No filePath supplied!");
  SM_ASSERT(buffer, "No buffer supplied!");
  auto file = fopen(filePath, "wb");
  if(!file)
  {
    SM_ERROR("Failed opening File: %s", filePath);
    return;
  }

  fwrite(buffer, sizeof(char), size, file);
  fclose(file);
}

inline char* read_file(const char* filePath, int* fileSize, BumpAllocator* bumpAllocator)
{
  char* file = 0;
  long fileSize2 = get_file_size(filePath);

  if(fileSize2)
  {
    char* buffer = bump_alloc(bumpAllocator, fileSize2 + 1);

    file = read_file(filePath, fileSize, buffer);
  }

  return file;
}

inline bool copy_file(const char* fileName, const char* outputName, char* buffer)
{
  int fileSize = 0;
  char* data = read_file(fileName, &fileSize, buffer);

  auto outputFile = fopen(outputName, "wb");
  if(!outputFile)
  {
    fprintf(stdout, "%d\n", errno);
    SM_ERROR("Failed opening File(open-output): %s", outputName);
    return false;
  }

  int result = fwrite(data, sizeof(char), fileSize, outputFile);
  if(!result)
  {
    SM_ERROR("Failed opening File(write-output): %s", outputName);
    return false;
  }

  fclose(outputFile);

  return true;
}

inline bool copy_file(const char* fileName, const char* outputName, BumpAllocator* bumpAllocator)
{
  char* file = 0;
  long fileSize2 = get_file_size(fileName);

  if(fileSize2)
  {
    char* buffer = bump_alloc(bumpAllocator, fileSize2 + 1);

    return copy_file(fileName, outputName, buffer);
  }

  return false;
}

// #############################################################################
//                           WAV File stuff
// #############################################################################
// Wave Files are seperated into chunks,
// struct chunk
// {
//   unsigned int id;
//   unsigned int size; // In bytes
//   ...
// }
// we are ASSUMING!!!! That we have a "Riff Chunk"
// followed by a "Format Chunk" followed by a
// "Data Chunk", this CAN! be wrong ofcourse
struct WAVHeader
{
  // Riff Chunk
	unsigned int riffChunkId;
	unsigned int riffChunkSize;
	unsigned int format;

  // Format Chunk
	unsigned int formatChunkId;
	unsigned int formatChunkSize;
	unsigned short audioFormat;
	unsigned short numChannels;
	unsigned int sampleRate;
	unsigned int byteRate;
	unsigned short blockAlign;
	unsigned short bitsPerSample;

  // Data Chunk
	unsigned char dataChunkId[4];
	unsigned int dataChunkSize;
};

struct WAVFile
{
	WAVHeader header;
	char dataBegin;
};

inline WAVFile* load_wav(char* path, BumpAllocator* bumpAllocator)
{
	int fileSize = 0;
	WAVFile* wavFile = (WAVFile*)read_file(path, &fileSize, bumpAllocator);
	if(!wavFile)
  {
    SM_ASSERT(0, "Failed to load Wave File: %s", path);
    return {};
  }

	SM_ASSERT(wavFile->header.numChannels == NUM_CHANNELS,
            "We only support 2 channels for now!");
	SM_ASSERT(wavFile->header.sampleRate == SAMPLE_RATE,
            "We only support 44100 sample rate for now!");

	SM_ASSERT(memcmp(&wavFile->header.dataChunkId, "data", 4) == 0,
						"WAV File not in propper format");

	return wavFile;
}
