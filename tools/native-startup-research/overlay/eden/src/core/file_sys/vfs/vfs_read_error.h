// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stdexcept>

namespace FileSys {
// A VFS backend could not produce valid requested bytes. This is distinct from
// an ordinary short read/EOF. IPC read boundaries translate only this category
// into an FS error; unrelated host exceptions retain their original behavior.
class VfsReadError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};
} // namespace FileSys
