#ifndef PROTON_BINARY_HPP
#define PROTON_BINARY_HPP

/*
 *
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 *
 */

#include "./internal/export.hpp"
#include "./types_fwd.hpp"

#include <proton/type_compat.h>

#include <iosfwd>
#include <string>
#include <vector>

/// @file
/// @copybrief proton::binary

namespace proton {

/// Arbitrary binary data.
class binary : public std::vector<uint8_t> {
  public:
    /// @name Constructors
    /// @{
    explicit binary() = default;
    using std::vector<value_type>::vector;
    explicit binary(const std::vector<value_type>& v) : std::vector<value_type>(v) {}
    explicit binary(const std::string& s) : std::vector<value_type>(s.begin(), s.end()) {}
    /// @}

    /// Convert to std::string
    operator std::string() const { return std::string(begin(), end()); }

    /// Assignment
    binary& operator=(const std::string& x) { assign(x.begin(), x.end()); return *this; }
};

/// Print a binary value
PN_CPP_EXTERN std::ostream& operator<<(std::ostream&, const binary&);

} // proton

/// Specialize std::hash so we can use proton::binary as a key for unordered datastructures
template <> struct std::hash<proton::binary> {
  std::size_t operator()(const proton::binary& k) const {
      std::string s{k};
      return std::hash<std::string>{}(s);
  }
};

#endif // PROTON_BINARY_HPP
