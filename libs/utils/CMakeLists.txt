# This file is part of the Flowee project
# Copyright (C) 2018 Tom Zander <tomz@freedommail.ch>
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.

include_directories(${LIBUTILS_INCLUDES})

add_library(flowee_utils STATIC
    amount.cpp
    allowed_args.cpp
    chainparamsbase.cpp
    clientversion.cpp
    compat/glibc_sanity.cpp
    compat/glibcxx_sanity.cpp
    compat/strnlen.cpp
    LogChannels.cpp
    Logger.cpp
    Message.cpp
    random.cpp
    rpcprotocol.cpp
    streaming/BufferPool.cpp
    streaming/ConstBuffer.cpp
    streaming/MessageBuilder.cpp
    streaming/MessageParser.cpp
    support/cleanse.cpp
    support/pagelocker.cpp
    sync.cpp
    uint256.cpp
    util.cpp
    utilmoneystr.cpp
    utilstrencodings.cpp
    utiltime.cpp
    WorkerThreads.cpp
)

add_dependencies(flowee_utils univalue)

target_link_libraries(flowee_utils flowee_interfaces flowee_crypto)
