//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef _LIBCPP___COMPARE_WEAK_ORDER
#define _LIBCPP___COMPARE_WEAK_ORDER

#include <__compare/compare_three_way.h>
#include <__compare/ordering.h>
#include <__compare/strong_order.h>
#include <__config>
#include <__type_traits/decay.h>
#include <__utility/forward.h>
#include <__utility/priority_tag.h>
#include <cmath>

#ifndef _LIBCPP_HAS_NO_PRAGMA_SYSTEM_HEADER
#  pragma GCC system_header
#endif

_LIBCPP_BEGIN_NAMESPACE_STD

#if _LIBCPP_STD_VER > 17

// [cmp.alg]
namespace __weak_order {
    struct __fn {
        template<class _Tp, class _Up>
            requires is_same_v<decay_t<_Tp>, decay_t<_Up>>
        _LIBCPP_HIDE_FROM_ABI static constexpr auto
        __go(_Tp&& __t, _Up&& __u, __priority_tag<3>)
            noexcept(noexcept(weak_ordering(weak_order(_VSTD::forward<_Tp>(__t), _VSTD::forward<_Up>(__u)))))
            -> decltype(      weak_ordering(weak_order(_VSTD::forward<_Tp>(__t), _VSTD::forward<_Up>(__u))))
            { return          weak_ordering(weak_order(_VSTD::forward<_Tp>(__t), _VSTD::forward<_Up>(__u))); }

        template<class _Tp, class _Up, class _Dp = decay_t<_Tp>>
            requires is_same_v<_Dp, decay_t<_Up>> && is_floating_point_v<_Dp>
        _LIBCPP_HIDE_FROM_ABI static constexpr weak_ordering
        __go(_Tp&& __t, _Up&& __u, __priority_tag<2>) noexcept
        {
            partial_ordering __po = (__t <=> __u);
            if (__po == partial_ordering::less) {
                return weak_ordering::less;
            } else if (__po == partial_ordering::equivalent) {
                return weak_ordering::equivalent;
            } else if (__po == partial_ordering::greater) {
                return weak_ordering::greater;
            } else {
                // Otherwise, at least one of them is a NaN.
                bool __t_is_nan = _VSTD::isnan(__t);
                bool __u_is_nan = _VSTD::isnan(__u);
                bool __t_is_negative = _VSTD::signbit(__t);
                bool __u_is_negative = _VSTD::signbit(__u);
                if (__t_is_nan && __u_is_nan) {
                    return (__u_is_negative <=> __t_is_negative);
                } else if (__t_is_nan) {
                    return __t_is_negative ? weak_ordering::less : weak_ordering::greater;
                } else {
                    return __u_is_negative ? weak_ordering::greater : weak_ordering::less;
                }
            }
        }

        template<class _Tp, class _Up>
            requires is_same_v<decay_t<_Tp>, decay_t<_Up>>
        _LIBCPP_HIDE_FROM_ABI static constexpr auto
        __go(_Tp&& __t, _Up&& __u, __priority_tag<1>)
            noexcept(noexcept(weak_ordering(compare_three_way()(_VSTD::forward<_Tp>(__t), _VSTD::forward<_Up>(__u)))))
            -> decltype(      weak_ordering(compare_three_way()(_VSTD::forward<_Tp>(__t), _VSTD::forward<_Up>(__u))))
            { return          weak_ordering(compare_three_way()(_VSTD::forward<_Tp>(__t), _VSTD::forward<_Up>(__u))); }

        template<class _Tp, class _Up>
            requires is_same_v<decay_t<_Tp>, decay_t<_Up>>
        _LIBCPP_HIDE_FROM_ABI static constexpr auto
        __go(_Tp&& __t, _Up&& __u, __priority_tag<0>)
            noexcept(noexcept(weak_ordering(_VSTD::strong_order(_VSTD::forward<_Tp>(__t), _VSTD::forward<_Up>(__u)))))
            -> decltype(      weak_ordering(_VSTD::strong_order(_VSTD::forward<_Tp>(__t), _VSTD::forward<_Up>(__u))))
            { return          weak_ordering(_VSTD::strong_order(_VSTD::forward<_Tp>(__t), _VSTD::forward<_Up>(__u))); }

        template<class _Tp, class _Up>
        _LIBCPP_HIDE_FROM_ABI constexpr auto operator()(_Tp&& __t, _Up&& __u) const
            noexcept(noexcept(__go(_VSTD::forward<_Tp>(__t), _VSTD::forward<_Up>(__u), __priority_tag<3>())))
            -> decltype(      __go(_VSTD::forward<_Tp>(__t), _VSTD::forward<_Up>(__u), __priority_tag<3>()))
            { return          __go(_VSTD::forward<_Tp>(__t), _VSTD::forward<_Up>(__u), __priority_tag<3>()); }
    };
} // namespace __weak_order

inline namespace __cpo {
    inline constexpr auto weak_order = __weak_order::__fn{};
} // namespace __cpo

#endif // _LIBCPP_STD_VER > 17

_LIBCPP_END_NAMESPACE_STD

#endif // _LIBCPP___COMPARE_WEAK_ORDER
