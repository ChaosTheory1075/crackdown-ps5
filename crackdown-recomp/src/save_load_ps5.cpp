// The campaign save's load, made dependable on the PS5 (Title Update 6 addresses).
//
// The save manager's load task (sub_825BC5B0) opens Crackdown.sav with FILE_FLAG_OVERLAPPED,
// sizes it, allocates a buffer and starts one overlapped ReadFile; its poll (sub_825BC7A8) takes
// the result with GetOverlappedResult, hands the buffer to the save manager (+336 data, +340
// size) and closes the file. Any failure on the way leaves the file open and the buffer empty,
// and the title then calls the save corrupt (sub_825B9580 validates the buffer). The PS5 logs
// show exactly that: the file opened, never closed, the save called corrupt - while
// ci/fw_perf_fixes.py's chunk check of the same file (sub_825B7598's rules) passes.
//
// The load task only takes a ReadFile that returns FALSE (the read pending) as started: one that
// returns TRUE, done at once, sends it to the failure path unless the last error happens to be
// ERROR_IO_PENDING. On the Xbox 360 the read is always pending; on the PS5 the file layer can
// finish it inside the call, and the title then called every save corrupt (crackdown_015.log:
// the save read in full, the task failed in the same millisecond).
//
// Here the load task's ReadFile is done synchronously through the file object (and, failing
// that, from the host file directly), its OVERLAPPED completed in place, and FALSE returned, so
// the poll's GetOverlappedResult finds the whole save; the size query falls back to the file's
// own size the same way. A load that fails anyway closes its file, which the title leaves open
// (overwriting the save then crashed). Every step is logged as [SAVE_LOAD].

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <system_error>

#include <rex/filesystem/devices/host_path_entry.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xfile.h>
#include <rex/system/xmemory.h>

#include "guest_memory.h"

REX_EXTERN(__imp__sub_827CB7F8);  // the title's CloseHandle

namespace {

using GuestFunction = void (*)(PPCContext&, uint8_t*);
using crackdown::guest::Load32;
using crackdown::guest::Store32;
using rex::X_STATUS;

// The load task object (r3 of sub_825BC5B0 / sub_825BC7A8).
constexpr uint32_t kTaskState = 4;       // 1 reading, 2 done
constexpr uint32_t kTaskManager = 12;    // the save manager: +336 data, +340 size
constexpr uint32_t kTaskFile = 16;       // the file handle (-1 none)
constexpr uint32_t kTaskBuffer = 20;
constexpr uint32_t kTaskSize = 24;
constexpr uint32_t kTaskOverlapped = 32;  // Internal, InternalHigh, Offset, OffsetHigh, hEvent
constexpr uint32_t kNoFile = 0xFFFFFFFF;

thread_local uint32_t t_load_task = 0;  // set while the load task runs on this thread

std::atomic<uint32_t> g_logged{0};
bool Log() { return g_logged.fetch_add(1, std::memory_order_relaxed) < 256; }

// The host file behind a guest file handle, or an empty path.
std::filesystem::path HostPath(rex::system::XFile* file) {
  auto* entry = dynamic_cast<rex::filesystem::HostPathEntry*>(file->entry());
  return entry ? entry->host_path() : std::filesystem::path();
}

// Reads length bytes at offset of the guest file handle into guest memory; false on failure.
bool ReadWhole(uint32_t handle, uint32_t buffer, uint32_t length, uint64_t offset, uint32_t* read) {
  *read = 0;
  auto file = REX_KERNEL_OBJECTS()->LookupObject<rex::system::XFile>(handle);
  if (!file) {
    REXLOG_WARN("[SAVE_LOAD] read: handle {:08X} is no file", handle);
    return false;
  }
  const X_STATUS status = file->ReadTransfer(buffer, length, offset, read);
  if (status == X_STATUS_SUCCESS && *read == length) {
    REXLOG_INFO("[SAVE_LOAD] read '{}': {} bytes at {} to {:08X}", file->entry()->absolute_path(), *read,
                offset, buffer);
    return true;
  }
  REXLOG_WARN("[SAVE_LOAD] read '{}': {} of {} bytes at {} to {:08X}, status {:08X}; reading the host file",
              file->entry()->absolute_path(), *read, length, offset, buffer, status);
  const std::filesystem::path path = HostPath(file.get());
  auto* memory = REX_KERNEL_MEMORY();
  if (path.empty() || !memory->LookupHeap(buffer) || !memory->LookupHeap(buffer + length - 1)) {
    REXLOG_ERROR("[SAVE_LOAD] read: no host file or guest buffer {:08X}+{:X}", buffer, length);
    return false;
  }
  std::FILE* host = std::fopen(path.string().c_str(), "rb");
  if (!host) {
    REXLOG_ERROR("[SAVE_LOAD] read: can't open {}", path.string());
    return false;
  }
  size_t got = 0;
  if (std::fseek(host, long(offset), SEEK_SET) == 0)
    got = std::fread(memory->TranslateVirtual(buffer), 1, length, host);
  std::fclose(host);
  *read = uint32_t(got);
  REXLOG_INFO("[SAVE_LOAD] read {}: {} of {} bytes", path.string(), got, length);
  return got == length;
}

// A failed load task's file, which the title leaves open: closed through the title's CloseHandle.
void CloseFailedLoad(PPCContext& ctx, uint8_t* base, uint32_t task) {
  const uint32_t file = Load32(base, task + kTaskFile);
  if (Load32(base, task + kTaskState) != 2 || file == kNoFile || !file) return;
  const PPCContext saved = ctx;
  ctx.r3.u64 = file;
  __imp__sub_827CB7F8(ctx, base);
  ctx = saved;
  Store32(base, task + kTaskFile, kNoFile);
  REXLOG_WARN("[SAVE_LOAD] load task {:08X} failed; closed its file {:08X}", task, file);
}

// The save manager's load task.
void SaveLoadStart(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  const uint32_t task = ctx.r3.u32;
  t_load_task = task;
  original(ctx, base);
  t_load_task = 0;
  if (Log())
    REXLOG_INFO("[SAVE_LOAD] load task {:08X}: state {}, file {:08X}, buffer {:08X}, size {}", task,
                Load32(base, task + kTaskState), Load32(base, task + kTaskFile), Load32(base, task + kTaskBuffer),
                Load32(base, task + kTaskSize));
  CloseFailedLoad(ctx, base, task);  // state 2 here is always a failure: the poll completes a load
}

// Its poll: logs how it ends.
void SaveLoadPoll(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  const uint32_t task = ctx.r3.u32;
  const uint32_t manager = Load32(base, task + kTaskManager);
  original(ctx, base);
  if (Load32(base, task + kTaskState) == 1) return;
  if (Log())
    REXLOG_INFO("[SAVE_LOAD] load task {:08X} done: save data {:08X}, {} bytes", task,
                Load32(base, manager + 336), Load32(base, manager + 340));
  if (Load32(base, task + kTaskBuffer)) CloseFailedLoad(ctx, base, task);  // handed over: buffer 0, file closed
}

// kernel32 ReadFile(handle, buffer, length, read, overlapped).
void ReadFileHook(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  if (!t_load_task) {
    original(ctx, base);
    return;
  }
  const uint32_t handle = ctx.r3.u32, buffer = ctx.r4.u32, length = ctx.r5.u32;
  const uint32_t read_out = ctx.r6.u32, overlapped = ctx.r7.u32;
  const uint64_t offset =
      overlapped ? (uint64_t(Load32(base, overlapped + 12)) << 32) | Load32(base, overlapped + 8) : 0;
  uint32_t read = 0;
  if (!ReadWhole(handle, buffer, length, offset, &read)) {
    REXLOG_ERROR("[SAVE_LOAD] the title's own ReadFile takes over");
    original(ctx, base);
    REXLOG_INFO("[SAVE_LOAD] ReadFile: {}, OVERLAPPED status {:08X}, {} bytes", ctx.r3.u32,
                overlapped ? Load32(base, overlapped) : 0, overlapped ? Load32(base, overlapped + 4) : 0);
    ctx.r3.u64 = 0;  // done or pending, the task must take it as pending (its poll reads the OVERLAPPED)
    return;
  }
  if (overlapped) {
    Store32(base, overlapped + 4, read);  // InternalHigh before Internal: the poll reads Internal first
    Store32(base, overlapped + 0, 0);     // STATUS_SUCCESS
  }
  if (read_out) Store32(base, read_out, read);
  ctx.r3.u64 = 0;  // "pending": the only answer the load task takes as a started read
}

// kernel32 GetFileSizeEx(handle, size).
void GetFileSizeHook(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  if (!t_load_task) {
    original(ctx, base);
    return;
  }
  const uint32_t handle = ctx.r3.u32, size_out = ctx.r4.u32;
  original(ctx, base);
  const uint64_t size = (uint64_t(Load32(base, size_out)) << 32) | Load32(base, size_out + 4);
  if (ctx.r3.u32) {
    REXLOG_INFO("[SAVE_LOAD] size: {} bytes", size);
    return;
  }
  auto file = REX_KERNEL_OBJECTS()->LookupObject<rex::system::XFile>(handle);
  std::error_code ec;
  const std::filesystem::path path = file ? HostPath(file.get()) : std::filesystem::path();
  const uintmax_t host_size = path.empty() ? uintmax_t(-1) : std::filesystem::file_size(path, ec);
  if (!file || path.empty() || ec) {
    REXLOG_ERROR("[SAVE_LOAD] size of handle {:08X} unknown", handle);
    return;
  }
  REXLOG_WARN("[SAVE_LOAD] size query failed; {} has {} bytes", path.string(), host_size);
  Store32(base, size_out, uint32_t(uint64_t(host_size) >> 32));
  Store32(base, size_out + 4, uint32_t(host_size));
  ctx.r3.u64 = 1;
}

// The save's validation (sub_825B9580): true when its chunks end at the terminator.
void SaveValidate(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  original(ctx, base);
  REXLOG_INFO("[SAVE_LOAD] save validation: {}", (ctx.r3.u32 & 0xFF) ? "valid" : "CORRUPT");
}

}  // namespace

#define CD_BIND(address, handler)   \
  REX_EXTERN(__imp__sub_##address); \
  REX_HOOK_RAW(sub_##address) { handler(ctx, base, __imp__sub_##address); }

CD_BIND(825BC5B0, SaveLoadStart)
CD_BIND(825BC7A8, SaveLoadPoll)
CD_BIND(827CC098, ReadFileHook)
CD_BIND(827CD1B8, GetFileSizeHook)
CD_BIND(825B9580, SaveValidate)
