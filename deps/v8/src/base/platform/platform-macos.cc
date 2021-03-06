// Copyright 2012 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Platform-specific code for MacOS goes here. For the POSIX-compatible
// parts, the implementation is in platform-posix.cc.

#include <dlfcn.h>
#include <mach/mach_init.h>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <sys/mman.h>
#include <unistd.h>

#include <AvailabilityMacros.h>

#include <errno.h>
#include <libkern/OSAtomic.h>
#include <mach/mach.h>
#include <mach/semaphore.h>
#include <mach/task.h>
#include <mach/vm_statistics.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/types.h>

#include <cmath>

#undef MAP_TYPE

#include "src/base/macros.h"
#include "src/base/platform/platform-posix-time.h"
#include "src/base/platform/platform-posix.h"
#include "src/base/platform/platform.h"

namespace v8 {
namespace base {


// Constants used for mmap.
// kMmapFd is used to pass vm_alloc flags to tag the region with the user
// defined tag 255 This helps identify V8-allocated regions in memory analysis
// tools like vmmap(1).
static const int kMmapFd = VM_MAKE_TAG(255);
static const off_t kMmapFdOffset = 0;

// static
void* OS::Allocate(const size_t requested, size_t* allocated,
                   OS::MemoryPermission access, void* hint) {
  const size_t msize = RoundUp(requested, getpagesize());
  int prot = GetProtectionFromMemoryPermission(access);
  void* mbase =
      mmap(hint, msize, prot, MAP_PRIVATE | MAP_ANON, kMmapFd, kMmapFdOffset);
  if (mbase == MAP_FAILED) return NULL;
  *allocated = msize;
  return mbase;
}

// static
void* OS::ReserveRegion(size_t size, void* hint) {
  void* result =
      mmap(hint, size, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE,
           kMmapFd, kMmapFdOffset);

  if (result == MAP_FAILED) return nullptr;

  return result;
}

// static
void* OS::ReserveAlignedRegion(size_t size, size_t alignment, void* hint,
                               size_t* allocated) {
  DCHECK((alignment % OS::AllocateAlignment()) == 0);
  hint = AlignedAddress(hint, alignment);
  size_t request_size = RoundUp(size + alignment,
                                static_cast<intptr_t>(OS::AllocateAlignment()));
  void* result = ReserveRegion(request_size, hint);
  if (result == nullptr) {
    *allocated = 0;
    return nullptr;
  }

  uint8_t* base = static_cast<uint8_t*>(result);
  uint8_t* aligned_base = RoundUp(base, alignment);
  DCHECK_LE(base, aligned_base);

  // Unmap extra memory reserved before and after the desired block.
  if (aligned_base != base) {
    size_t prefix_size = static_cast<size_t>(aligned_base - base);
    OS::Free(base, prefix_size);
    request_size -= prefix_size;
  }

  size_t aligned_size = RoundUp(size, OS::AllocateAlignment());
  DCHECK_LE(aligned_size, request_size);

  if (aligned_size != request_size) {
    size_t suffix_size = request_size - aligned_size;
    OS::Free(aligned_base + aligned_size, suffix_size);
    request_size -= suffix_size;
  }

  DCHECK(aligned_size == request_size);

  *allocated = aligned_size;
  return static_cast<void*>(aligned_base);
}

// static
bool OS::CommitRegion(void* address, size_t size, bool is_executable) {
  int prot = PROT_READ | PROT_WRITE | (is_executable ? PROT_EXEC : 0);
  if (MAP_FAILED == mmap(address,
                         size,
                         prot,
                         MAP_PRIVATE | MAP_ANON | MAP_FIXED,
                         kMmapFd,
                         kMmapFdOffset)) {
    return false;
  }
  return true;
}

// static
bool OS::UncommitRegion(void* address, size_t size) {
  return mmap(address,
              size,
              PROT_NONE,
              MAP_PRIVATE | MAP_ANON | MAP_NORESERVE | MAP_FIXED,
              kMmapFd,
              kMmapFdOffset) != MAP_FAILED;
}

// static
bool OS::ReleaseRegion(void* address, size_t size) {
  return munmap(address, size) == 0;
}

// static
bool OS::ReleasePartialRegion(void* address, size_t size) {
  return munmap(address, size) == 0;
}

// static
bool OS::HasLazyCommits() { return true; }

std::vector<OS::SharedLibraryAddress> OS::GetSharedLibraryAddresses() {
  std::vector<SharedLibraryAddress> result;
  unsigned int images_count = _dyld_image_count();
  for (unsigned int i = 0; i < images_count; ++i) {
    const mach_header* header = _dyld_get_image_header(i);
    if (header == NULL) continue;
#if V8_HOST_ARCH_X64
    uint64_t size;
    char* code_ptr = getsectdatafromheader_64(
        reinterpret_cast<const mach_header_64*>(header), SEG_TEXT, SECT_TEXT,
        &size);
#else
    unsigned int size;
    char* code_ptr = getsectdatafromheader(header, SEG_TEXT, SECT_TEXT, &size);
#endif
    if (code_ptr == NULL) continue;
    const intptr_t slide = _dyld_get_image_vmaddr_slide(i);
    const uintptr_t start = reinterpret_cast<uintptr_t>(code_ptr) + slide;
    result.push_back(SharedLibraryAddress(_dyld_get_image_name(i), start,
                                          start + size, slide));
  }
  return result;
}

void OS::SignalCodeMovingGC(void* hint) {}

TimezoneCache* OS::CreateTimezoneCache() {
  return new PosixDefaultTimezoneCache();
}

}  // namespace base
}  // namespace v8
