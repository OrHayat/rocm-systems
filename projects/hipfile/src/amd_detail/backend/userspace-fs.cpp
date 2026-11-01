/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include "backend/userspace-fs.h"

#include "backend.h"
#include "buffer.h"
#include "file.h"
#include "hipfile.h"
#include "io.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <system_error>

using namespace hipFile;

int
UserspaceFs::score(const std::shared_ptr<IFile> &file, const std::shared_ptr<IBuffer> &buffer, size_t size,
                   hoff_t file_offset, hoff_t buffer_offset) const
{
    (void)buffer;
    (void)size;
    (void)file_offset;
    (void)buffer_offset;

    const hipFileFSOps_t *ops{file->fsOps()};
    if (ops == nullptr || ops->read == nullptr || ops->write == nullptr) {
        return -1;
    }
    return Score;
}

ssize_t
UserspaceFs::_io_impl(IoType type, std::shared_ptr<IFile> file, std::shared_ptr<IBuffer> buffer, size_t size,
                      hoff_t file_offset, hoff_t buffer_offset)
{
    const hipFileFSOps_t *ops{file->fsOps()};
    if (ops == nullptr || ops->read == nullptr || ops->write == nullptr) {
        throw BackendDisabled();
    }

    size = std::min(size, hipFile::getMaxRwCount());

    if (!paramsValid(buffer, size, file_offset, buffer_offset)) {
        throw std::invalid_argument("The selected file or buffer region is invalid");
    }

    char   *buf{static_cast<char *>(buffer->getBuffer()) + buffer_offset};
    void   *handle{file->fsHandle()};
    ssize_t nbytes{type == IoType::Read ? ops->read(handle, buf, size, file_offset, nullptr)
                                        : ops->write(handle, buf, size, file_offset, nullptr)};

    if (nbytes < 0) {
        throw std::system_error(static_cast<int>(-nbytes), std::generic_category());
    }
    // A filesystem that claims more than it was asked for has a bug; don't pass that on.
    if (static_cast<size_t>(nbytes) > size) {
        throw std::system_error(EIO, std::generic_category());
    }
    return nbytes;
}

void
UserspaceFs::async_io(IoType, std::shared_ptr<IFile>, std::shared_ptr<IBuffer>, size_t *, hoff_t *, hoff_t *,
                      ssize_t *, std::shared_ptr<IStream>)
{
    throw std::runtime_error("UserspaceFs::async_io is not implemented");
}

void
UserspaceFs::enqueueAsyncIo(IoType, std::shared_ptr<IFile>, std::shared_ptr<IBuffer>, size_t *, hoff_t *,
                            hoff_t *, ssize_t *, std::shared_ptr<IStream>,
                            std::shared_ptr<AsyncFailoverState>)
{
    throw std::runtime_error("UserspaceFs::enqueueAsyncIo is not implemented");
}
