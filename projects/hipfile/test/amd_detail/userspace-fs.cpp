/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include "backend.h"
#include "backend/userspace-fs.h"
#include "hipfile.h"
#include "hipfile-warnings.h"
#include "io.h"
#include "mbuffer.h"
#include "mfile.h"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <memory>
#include <stdexcept>
#include <system_error>

using namespace hipFile;
using namespace testing;
using namespace std;

// Put tests inside the macros to suppress the global constructor warnings
HIPFILE_WARN_NO_GLOBAL_CTOR_OFF

namespace {

// What the last callback received, and what the next one returns. Each test runs in its own process.
struct FsCall {
    void              *handle;
    const char        *buf;
    size_t             size;
    hoff_t             offset;
    hipFileRDMAInfo_t *rdma_info;
    bool               write;
};

FsCall  last_call{};
ssize_t next_result{0};

ssize_t
fsRead(void *handle, char *buf, size_t size, hoff_t offset, hipFileRDMAInfo_t *rdma_info)
{
    last_call = {handle, buf, size, offset, rdma_info, false};
    return next_result;
}

ssize_t
fsWrite(void *handle, const char *buf, size_t size, hoff_t offset, hipFileRDMAInfo_t *rdma_info)
{
    last_call = {handle, buf, size, offset, rdma_info, true};
    return next_result;
}

const hipFileFSOps_t fs_ops{nullptr, nullptr, nullptr, fsRead, fsWrite};
const hipFileFSOps_t read_only_ops{nullptr, nullptr, nullptr, fsRead, nullptr};

} // namespace

struct UserspaceFsTest : public testing::Test {
    int                             fs_file{};
    const size_t                    io_size{2048};
    const hoff_t                    file_offset{4096};
    const hoff_t                    buffer_offset{1024};
    array<char, 8192>               data{};
    shared_ptr<StrictMock<MFile>>   mfile{make_shared<StrictMock<MFile>>()};
    shared_ptr<StrictMock<MBuffer>> mbuffer{make_shared<StrictMock<MBuffer>>()};

    void expectIo(const hipFileFSOps_t *ops)
    {
        EXPECT_CALL(*mfile, fsOps).WillRepeatedly(Return(ops));
        EXPECT_CALL(*mfile, fsHandle).WillRepeatedly(Return(&fs_file));
        EXPECT_CALL(*mbuffer, getBuffer).WillRepeatedly(Return(data.data()));
        EXPECT_CALL(*mbuffer, getLength).WillRepeatedly(Return(data.size()));
    }
};

TEST_F(UserspaceFsTest, ScoreAcceptsFileWithFsOps)
{
    EXPECT_CALL(*mfile, fsOps).WillOnce(Return(&fs_ops));
    ASSERT_EQ(UserspaceFs().score(mfile, mbuffer, io_size, file_offset, buffer_offset), UserspaceFs::Score);
}

TEST_F(UserspaceFsTest, ScoreRejectsKernelFile)
{
    EXPECT_CALL(*mfile, fsOps).WillOnce(Return(nullptr));
    ASSERT_EQ(UserspaceFs().score(mfile, mbuffer, io_size, file_offset, buffer_offset), -1);
}

TEST_F(UserspaceFsTest, ScoreRejectsFsOpsWithoutBothCallbacks)
{
    EXPECT_CALL(*mfile, fsOps).WillOnce(Return(&read_only_ops));
    ASSERT_EQ(UserspaceFs().score(mfile, mbuffer, io_size, file_offset, buffer_offset), -1);
}

TEST_F(UserspaceFsTest, ReadPassesHandleBufferRegionAndNullRdmaInfo)
{
    expectIo(&fs_ops);
    next_result = static_cast<ssize_t>(io_size);

    ASSERT_EQ(UserspaceFs().io(IoType::Read, mfile, mbuffer, io_size, file_offset, buffer_offset),
              static_cast<ssize_t>(io_size));
    EXPECT_FALSE(last_call.write);
    EXPECT_EQ(last_call.handle, &fs_file);
    EXPECT_EQ(last_call.buf, data.data() + buffer_offset);
    EXPECT_EQ(last_call.size, io_size);
    EXPECT_EQ(last_call.offset, file_offset);
    EXPECT_EQ(last_call.rdma_info, nullptr);
}

TEST_F(UserspaceFsTest, WriteCallsWriteCallback)
{
    expectIo(&fs_ops);
    next_result = 100;

    ASSERT_EQ(UserspaceFs().io(IoType::Write, mfile, mbuffer, io_size, file_offset, buffer_offset), 100);
    EXPECT_TRUE(last_call.write);
    EXPECT_EQ(last_call.buf, data.data() + buffer_offset);
}

TEST_F(UserspaceFsTest, NegativeResultThrowsItsErrno)
{
    expectIo(&fs_ops);
    next_result = -ENOSPC;

    try {
        UserspaceFs().io(IoType::Write, mfile, mbuffer, io_size, file_offset, buffer_offset);
        FAIL() << "expected std::system_error";
    }
    catch (const std::system_error &e) {
        EXPECT_EQ(e.code().value(), ENOSPC);
    }
}

TEST_F(UserspaceFsTest, ResultLargerThanRequestThrowsEio)
{
    expectIo(&fs_ops);
    next_result = static_cast<ssize_t>(io_size) + 1;

    try {
        UserspaceFs().io(IoType::Read, mfile, mbuffer, io_size, file_offset, buffer_offset);
        FAIL() << "expected std::system_error";
    }
    catch (const std::system_error &e) {
        EXPECT_EQ(e.code().value(), EIO);
    }
}

TEST_F(UserspaceFsTest, IoThrowsIfOpCouldOverrunBuffer)
{
    expectIo(&fs_ops);
    ASSERT_THROW(UserspaceFs().io(IoType::Read, mfile, mbuffer, data.size(), file_offset, buffer_offset),
                 std::invalid_argument);
}

TEST_F(UserspaceFsTest, IoThrowsOnNegativeFileOffset)
{
    expectIo(&fs_ops);
    ASSERT_THROW(UserspaceFs().io(IoType::Read, mfile, mbuffer, io_size, -1, buffer_offset),
                 std::invalid_argument);
}

TEST_F(UserspaceFsTest, IoRejectedWithoutBothCallbacks)
{
    expectIo(&read_only_ops);
    ASSERT_THROW(UserspaceFs().io(IoType::Read, mfile, mbuffer, io_size, file_offset, buffer_offset),
                 BackendDisabled);
}

TEST_F(UserspaceFsTest, AsyncIoIsNotSupported)
{
    size_t  size{io_size};
    hoff_t  foff{file_offset};
    hoff_t  boff{buffer_offset};
    ssize_t nbytes{0};

    ASSERT_THROW(UserspaceFs().async_io(IoType::Read, mfile, mbuffer, &size, &foff, &boff, &nbytes, nullptr),
                 std::runtime_error);
    ASSERT_THROW(UserspaceFs().enqueueAsyncIo(IoType::Read, mfile, mbuffer, &size, &foff, &boff, &nbytes,
                                              nullptr, nullptr),
                 std::runtime_error);
}

HIPFILE_WARN_NO_GLOBAL_CTOR_ON
