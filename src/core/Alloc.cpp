/*
 *  Copyright (C) 2019 KeePassXC Team <team@keepassxc.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 2 or (at your option)
 *  version 3 of the License.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "config-keepassx.h"
#include <QtGlobal>
#include <botan/mem_ops.h>
#include <cstdlib>
#if defined(Q_OS_MACOS)
#include <malloc/malloc.h>
#elif defined(Q_OS_FREEBSD)
#include <malloc_np.h>
#elif defined(HAVE_MALLOC_H)
#include <malloc.h>
#endif

#if defined(NDEBUG) && !defined(__cpp_sized_deallocation)
#warning "OpenVireo is being compiled without sized deallocation support. Deletes may be slow."
#endif

namespace
{
    void secureFree(void* ptr, std::size_t size) noexcept
    {
        Botan::secure_scrub_memory(ptr, size);
        std::free(ptr);
    }
} // namespace

/**
 * Custom sized delete operator which securely zeroes out allocated
 * memory before freeing it (requires C++14 sized deallocation support).
 */
void operator delete(void* ptr, std::size_t size) noexcept
{
    if (!ptr) {
        return;
    }

    secureFree(ptr, size);
}

void operator delete[](void* ptr, std::size_t size) noexcept
{
    ::operator delete(ptr, size);
}

/**
 * Custom delete operator which securely zeroes out
 * allocated memory before freeing it.
 */
void operator delete(void* ptr) noexcept
{
    if (!ptr) {
        return;
    }

    // Query the allocation size before handing the pointer off to be freed
#if defined(Q_OS_WIN)
    const std::size_t size = _msize(ptr);
    secureFree(ptr, size);
#elif defined(Q_OS_MACOS)
    const std::size_t size = malloc_size(ptr);
    secureFree(ptr, size);
#elif defined(HAVE_MALLOC_USABLE_SIZE)
    const std::size_t size = malloc_usable_size(ptr);
    secureFree(ptr, size);
#else
    // whatever OS this is, give up and simply free stuff
    std::free(ptr);
#endif
}

void operator delete[](void* ptr) noexcept
{
    ::operator delete(ptr);
}

// clang-format versions less than 10.0 refuse to put a space before "noexcept"
// clang-format off
/**
 * Custom insecure delete operator that does not zero out memory before
 * freeing a buffer. Can be used for better performance.
 */
void operator delete(void* ptr, bool) noexcept
{
    std::free(ptr);
}
// clang-format on

void operator delete[](void* ptr, bool) noexcept
{
    ::operator delete(ptr, false);
}
