#pragma once

#include <type_traits>

namespace creatorx {

template <typename T>
struct is_reflectable : std::false_type {};

} // namespace creatorx
