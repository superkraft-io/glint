#pragma once

/**
 * glint_utils.hpp
 * Small numeric helpers used across glint.
 *
 * map(value, from_a, to_a, from_b, to_b)
 *   Maps value from [from_a, to_a] into [from_b, to_b].
 *
 * Examples:
 *   map(0.5f, 0.f, 1.f, 0.f, 100.f) -> 50.f
 *   map(64, 0, 127, -1.f, 1.f) -> about 0.007874
 */

#include <type_traits>

namespace glint_utils {

template <typename Result,
          typename Value,
          typename FromA,
          typename ToA,
          typename FromB,
          typename ToB>
struct map_traits
{
  using raw_type = std::common_type_t<Result, Value, FromA, ToA, FromB, ToB>;
  using calc_type =
      std::conditional_t<std::is_floating_point_v<raw_type>, raw_type, double>;
  using return_type = Result;
};

template <typename Value,
          typename FromA,
          typename ToA,
          typename FromB,
          typename ToB>
struct map_traits<void, Value, FromA, ToA, FromB, ToB>
{
  using raw_type = std::common_type_t<Value, FromA, ToA, FromB, ToB>;
  using calc_type =
      std::conditional_t<std::is_floating_point_v<raw_type>, raw_type, double>;
  using return_type = calc_type;
};

template <typename Result = void,
          typename Value,
          typename FromA,
          typename ToA,
          typename FromB,
          typename ToB>
inline auto map(Value value, FromA from_a, ToA to_a, FromB from_b, ToB to_b)
{
  using traits = map_traits<Result, Value, FromA, ToA, FromB, ToB>;
  using calc_type = typename traits::calc_type;
  using return_type = typename traits::return_type;

  const calc_type input_min = static_cast<calc_type>(from_a);
  const calc_type input_max = static_cast<calc_type>(to_a);
  const calc_type output_min = static_cast<calc_type>(from_b);
  const calc_type output_max = static_cast<calc_type>(to_b);

  const calc_type input_span = input_max - input_min;
  if (input_span == calc_type(0))
    return static_cast<return_type>(output_min);

  const calc_type t = (static_cast<calc_type>(value) - input_min) / input_span;
  return static_cast<return_type>(output_min + t * (output_max - output_min));
}

} // namespace glint_utils
